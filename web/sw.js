// Service worker: after the first load, this page never needs the network again.
//
// That is not a nicety bolted on — it is what this thing actually is. In mesh mode the
// server never touches audio and never sees a message; every word crosses the air between
// speakers and microphones. The network's only job is to hand you the page once. After
// that it is dead weight, and the page should keep working with WiFi off, in a basement,
// on a plane.
//
// Cache-first on purpose. A stale-but-working page beats a fresh-but-unreachable one when
// the whole point is that you are not supposed to need the network. Updates are picked up
// in the background on the next load that does happen to have connectivity.

const CACHE = 'uchat-v1';

// Everything required to run. If any of these is missing the app is dead, so the install
// deliberately fails rather than half-caching and pretending to be offline-ready.
const ASSETS = [
  './',
  './index.html',
  './uchat.js',
  './uchat.wasm',
  './manifest.json',
];

self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(CACHE)
      .then(c => c.addAll(ASSETS))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  if (e.request.method !== 'GET') return;

  e.respondWith(
    caches.match(e.request).then(hit => {
      // Refresh in the background when there IS a network, so the next load is current.
      // Failure here is expected and ignored — being offline is the normal case.
      const fresh = fetch(e.request)
        .then(res => {
          if (res && res.ok && res.type === 'basic')
            caches.open(CACHE).then(c => c.put(e.request, res.clone()));
          return res;
        })
        .catch(() => null);

      return hit || fresh.then(r => r || new Response('offline and not cached', { status: 504 }));
    })
  );
});
