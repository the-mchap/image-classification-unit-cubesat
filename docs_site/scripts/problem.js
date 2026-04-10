(() => {
  'use strict';

  const SVG_NS = 'http://www.w3.org/2000/svg';

  // =========================================================
  // DOM refs
  // =========================================================

  const svg = document.querySelector('.problem-diagram');
  if (!svg) return;

  const orbitPath = document.getElementById('orbit-path');
  const satellite = document.getElementById('satellite');
  const dataLink = document.getElementById('data-link');
  const capturedEl = document.getElementById('stat-captured');
  const receivedEl = document.getElementById('stat-received');
  const annotationEl = document.getElementById('problem-annotation');

  if (!orbitPath || !satellite) return;

  const pathLength = orbitPath.getTotalLength();
  const EARTH_X = 450;
  const EARTH_Y = 260;

  // =========================================================
  // Tuning
  // =========================================================

  const ORBIT_MS = 10000;
  const CAPTURE_MS = 2800;
  const IMGS_PER_CAPTURE = 5;
  const DELIVERY_RATIO = 0.12;

  // =========================================================
  // State
  // =========================================================

  let captured = 0;
  let received = 0;
  let lastCapture = -CAPTURE_MS;
  let images = [];
  let running = false;
  let initialized = false;

  // =========================================================
  // Stars
  // =========================================================

  function createStars(count) {
    const frag = document.createDocumentFragment();
    for (let i = 0; i < count; i++) {
      const c = document.createElementNS(SVG_NS, 'circle');
      c.classList.add('star');
      c.setAttribute('cx', Math.random() * 900);
      c.setAttribute('cy', Math.random() * 520);
      c.setAttribute('r', (Math.random() * 1 + 0.3).toFixed(1));
      c.style.animationDelay = `${(Math.random() * 5).toFixed(1)}s`;
      frag.appendChild(c);
    }
    svg.insertBefore(frag, svg.firstChild);
  }

  // =========================================================
  // Image squares
  // =========================================================

  function spawnImages(pos, ts) {
    for (let i = 0; i < IMGS_PER_CAPTURE; i++) {
      const rect = document.createElementNS(SVG_NS, 'rect');
      rect.classList.add('img-square');

      const angle = (i / IMGS_PER_CAPTURE) * Math.PI * 2;
      const spread = 22 + Math.random() * 12;
      const willDeliver = Math.random() < DELIVERY_RATIO;

      if (willDeliver) rect.classList.add('img-delivered');
      svg.appendChild(rect);

      images.push({
        el: rect,
        sx: pos.x + Math.cos(angle) * spread,
        sy: pos.y + Math.sin(angle) * spread,
        dx: (Math.random() - 0.5) * 50,
        dy: -15 - Math.random() * 25,
        willDeliver,
        born: ts + i * 80,
        life: willDeliver ? 2400 : 1600 + Math.random() * 800,
      });
    }

    captured += IMGS_PER_CAPTURE;
    if (capturedEl) capturedEl.textContent = captured;
  }

  function easeInOut(t) {
    return t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2;
  }

  function updateImages(ts) {
    images = images.filter((img) => {
      const age = ts - img.born;
      if (age < 0) return true;

      const p = age / img.life;
      if (p >= 1) {
        img.el.remove();
        if (img.willDeliver) {
          received++;
          if (receivedEl) receivedEl.textContent = received;
        }
        return false;
      }

      let x, y, opacity;

      if (img.willDeliver) {
        x = img.sx + (EARTH_X - img.sx) * easeInOut(p);
        y = img.sy + (EARTH_Y - img.sy) * easeInOut(p);
        opacity =
          p < 0.1 ? (p / 0.1) * 0.9 : p > 0.85 ? ((1 - p) / 0.15) * 0.9 : 0.9;
      } else {
        x = img.sx + img.dx * p;
        y = img.sy + img.dy * p;
        opacity = p < 0.08 ? (p / 0.08) * 0.55 : (1 - p) * 0.55;
      }

      img.el.setAttribute('x', x - 4);
      img.el.setAttribute('y', y - 4);
      img.el.style.opacity = opacity;

      return true;
    });
  }

  // =========================================================
  // Camera flash
  // =========================================================

  const satFlash = satellite.querySelector('.sat-flash');

  if (satFlash) {
    satFlash.addEventListener('animationend', () => {
      satFlash.classList.remove('flash-active');
    });
  }

  function triggerFlash() {
    if (!satFlash) return;
    if (satFlash.classList.contains('flash-active')) {
      satFlash.classList.remove('flash-active');
      void satFlash.offsetWidth;
    }
    satFlash.classList.add('flash-active');
  }

  // =========================================================
  // Annotation cycling
  // =========================================================

  const annotations = [
    '// thousands of captures per orbit',
    '// downlink: ~1200 bps',
    '// most data never reaches Earth',
  ];
  let annIndex = 0;
  let annInterval = null;

  function cycleAnnotation() {
    if (!annotationEl) return;
    annotationEl.classList.add('ann-fading');
    setTimeout(() => {
      annIndex = (annIndex + 1) % annotations.length;
      annotationEl.textContent = annotations[annIndex];
      annotationEl.classList.remove('ann-fading');
    }, 400);
  }

  // =========================================================
  // Main loop
  // =========================================================

  function animate(ts) {
    if (!running) return;

    // Orbit + rotation toward Earth
    const progress = (ts % ORBIT_MS) / ORBIT_MS;
    const pt = orbitPath.getPointAtLength(progress * pathLength);
    const angle = Math.atan2(EARTH_Y - pt.y, EARTH_X - pt.x);
    const deg = (angle * 180) / Math.PI - 90;
    satellite.setAttribute(
      'transform',
      `translate(${pt.x}, ${pt.y}) rotate(${deg})`,
    );

    // Data link
    if (dataLink) {
      dataLink.setAttribute('x1', EARTH_X);
      dataLink.setAttribute('y1', EARTH_Y);
      dataLink.setAttribute('x2', pt.x);
      dataLink.setAttribute('y2', pt.y);
    }

    // Capture cycle
    if (ts - lastCapture >= CAPTURE_MS) {
      lastCapture = ts;
      triggerFlash();
      spawnImages(pt, ts);
    }

    updateImages(ts);
    requestAnimationFrame(animate);
  }

  // =========================================================
  // Visibility-gated init
  // =========================================================

  const observer = new IntersectionObserver(
    (entries) => {
      if (entries[0].isIntersecting) {
        if (!initialized) {
          createStars(50);
          if (annotationEl) annotationEl.textContent = annotations[0];
          initialized = true;
        }
        if (!running) {
          running = true;
          requestAnimationFrame(animate);
        }
        if (!annInterval && annotationEl) {
          annInterval = setInterval(cycleAnnotation, 4000);
        }
      } else {
        running = false;
        images.forEach((img) => img.el.remove());
        images = [];
        if (annInterval) {
          clearInterval(annInterval);
          annInterval = null;
        }
      }
    },
    { threshold: 0.05 },
  );

  observer.observe(svg);
})();
