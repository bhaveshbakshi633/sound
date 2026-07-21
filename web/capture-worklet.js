// Microphone capture on the audio-render thread.
//
// Replaces the deprecated ScriptProcessorNode. ScriptProcessorNode ran its callback on the
// MAIN thread: every 2048 samples the UI had to stop whatever it was doing to hand audio to
// the DSP, and any jank in the page (a reflow, a GC pause) showed up as dropped microphone
// frames — dropped frames are dropped symbols are lost messages. An AudioWorkletProcessor
// runs on the audio thread, so capture is never blocked by the page.
//
// The DSP itself still lives on the main thread (the WASM module is loaded there). This
// processor's only job is to forward captured samples across the port. It batches to ~2048
// samples before posting so the main thread gets one message per ~43 ms instead of one per
// 128-sample quantum (~2.7 ms) — same data, ~16× fewer messages.
//
// `capturing` gates forwarding at the source: when the page is not focused we stop posting
// entirely (see the focus-gating in mesh.html), so no microphone audio leaves this thread
// while the user isn't looking. Toggled by a message on the port.
class CaptureProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.BATCH = 2048;
    this.buf = new Float32Array(this.BATCH);
    this.n = 0;
    this.capturing = true;
    this.port.onmessage = (e) => {
      if (e.data && e.data.type === 'capture') this.capturing = !!e.data.on;
    };
  }

  process(inputs) {
    const input = inputs[0];
    // No input connected yet, or capture paused — do nothing, but stay alive (return true).
    if (!this.capturing || !input || input.length === 0) return true;
    const ch = input[0];
    if (!ch) return true;

    for (let i = 0; i < ch.length; i++) {
      this.buf[this.n++] = ch[i];
      if (this.n === this.BATCH) {
        // Transfer the buffer to the main thread (zero-copy) and start a fresh one.
        this.port.postMessage(this.buf, [this.buf.buffer]);
        this.buf = new Float32Array(this.BATCH);
        this.n = 0;
      }
    }
    return true;
  }
}

registerProcessor('capture', CaptureProcessor);
