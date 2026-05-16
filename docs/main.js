/* aoko client docs - lightweight interactions */

/* Status date */
(function () {
  const el = document.getElementById('status-date');
  if (!el) return;
  const now = new Date();
  el.textContent = now.toLocaleDateString('en-US', { month: 'long', day: 'numeric', year: 'numeric' });
})();

/* Scroll reveal */
(function () {
  const items = document.querySelectorAll(
    '.section-head, .entry, .col-num, .status-list, .dl-block, .hero-logo, .tagline, .hero-btns, .hero-preview'
  );

  items.forEach((el) => el.classList.add('reveal'));

  const observer = new IntersectionObserver((entries) => {
    entries.forEach((entry) => {
      if (!entry.isIntersecting) return;
      entry.target.classList.add('visible');
      observer.unobserve(entry.target);
    });
  }, { threshold: 0.12 });

  items.forEach((el) => observer.observe(el));
})();
