(() => {
  'use strict';

  // =========================================================
  // Scroll-triggered fade-in
  // =========================================================

  /** Pauses CSS animations until elements scroll into view. */
  function initScrollReveal() {
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            entry.target.style.animationPlayState = 'running';
            observer.unobserve(entry.target);
          }
        });
      },
      { threshold: 0.1 },
    );

    document.querySelectorAll('.diagram-section').forEach((el) => {
      el.style.animationPlayState = 'paused';
      observer.observe(el);
    });
  }

  // =========================================================
  // Footer quote selector
  // =========================================================

  /** @type {string[]} Raw footer quotes */
  const quotes = [
    'Are you <strong>the fire</strong> or just another flame?',
    'Sometimes <strong>you gotta run</strong> before you can walk',
    "You can't start a fire without a <strong>spark</strong>",
    '<strong>Sweat</strong> saves blood',
    'That which does not kill us makes us... <strong>stranger</strong>',
    'By <strong>endurance</strong> we conquer',
    'I would rather die <strong>on my feet</strong> than live on my knees',
    'Fear cuts <strong>deeper</strong> than swords',
    'Stay hungry. Stay <strong>foolish</strong>',
    'To be is to be <strong>perceived</strong>',
    'The struggle itself toward the heights is enough to <strong>fill a man’s heart</strong>',
    'Man is nothing else but what he makes of <strong>himself</strong>',
    'Between stimulus and <strong>response</strong> there is a space',
    'To <strong>endure</strong> is greater than to dare',
    'We see the world not as it is, but as <strong>we are</strong>',
  ];

  /** Picks a random quote and injects it into the footer element if present. */
  function initFooterQuote() {
    const quoteEl = document.getElementById('footer-quote');
    if (!quoteEl) return;
    const index = Math.floor(Math.random() * quotes.length);
    quoteEl.innerHTML = quotes[index];
    requestAnimationFrame(() => {
      quoteEl.classList.add('show');
    });
  }

  // =========================================================
  // Init
  // =========================================================

  initScrollReveal();
  initFooterQuote();
})();
