/* LegoClicker — main.js */

/* ── Matrix Rain ─────────────────────────── */
(function () {
  const canvas = document.getElementById('matrix');
  const ctx = canvas.getContext('2d');

  const CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%^&*()アイウエオカキクケコサシスセソタチツテト';

  let cols, drops;
  const FONT_SIZE = 14;

  function resize() {
    canvas.width  = window.innerWidth;
    canvas.height = window.innerHeight;
    cols  = Math.floor(canvas.width / FONT_SIZE);
    drops = Array(cols).fill(1).map(() => Math.random() * -100);
  }

  resize();
  window.addEventListener('resize', resize);

  function draw() {
    ctx.fillStyle = 'rgba(5,5,8,0.05)';
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    ctx.font = FONT_SIZE + 'px "Share Tech Mono", monospace';

    for (let i = 0; i < cols; i++) {
      const char = CHARS[Math.floor(Math.random() * CHARS.length)];
      const y = drops[i] * FONT_SIZE;

      // head char is brighter
      const head = drops[i] * FONT_SIZE < canvas.height * 0.15;
      ctx.fillStyle = head ? '#f0d8ff' : '#b28cff';
      ctx.globalAlpha = head ? 0.9 : 0.4 + Math.random() * 0.3;
      ctx.fillText(char, i * FONT_SIZE, y);
      ctx.globalAlpha = 1;

      if (y > canvas.height && Math.random() > 0.975) {
        drops[i] = 0;
      }
      drops[i] += 0.5;
    }
  }

  setInterval(draw, 45);
})();

/* ── Status date ─────────────────────────── */
(function () {
  const el = document.getElementById('status-date');
  if (!el) return;
  const now = new Date();
  el.textContent = now.toLocaleDateString('en-US', { month: 'long', day: 'numeric', year: 'numeric' });
})();

/* ── CPS counter flicker ─────────────────── */
(function () {
  const el = document.getElementById('cps-counter');
  if (!el) return;
  const base = 16;
  setInterval(() => {
    const jitter = Math.floor(Math.random() * 3) - 1; // -1 0 +1
    el.textContent = base + jitter;
  }, 800);
})();

/* ── Scroll Reveal ───────────────────────── */
(function () {
  const items = document.querySelectorAll(
    '.feature-card, .status-card, .arch-step, .setup-block, .dl-box, .section-header'
  );

  items.forEach(el => el.classList.add('reveal'));

  const observer = new IntersectionObserver((entries) => {
    entries.forEach(e => {
      if (e.isIntersecting) {
        e.target.classList.add('visible');
        observer.unobserve(e.target);
      }
    });
  }, { threshold: 0.12 });

  items.forEach(el => observer.observe(el));
})();

/* ── Glitch title on hover ───────────────── */
(function () {
  const el = document.querySelector('.hero h1');
  if (!el) return;

  const originalHtml = el.innerHTML;
  const originalText = el.textContent || '';
  const glitchChars = '!<>-_\\/[]{}—=+*^?#______█▓▒░';
  let glitchTimer = null;

  function glitch() {
    if (glitchTimer !== null) return;
    let iter = 0;
    glitchTimer = setInterval(() => {
      el.textContent = originalText.split('').map((char, idx) => {
        if (idx < iter || char === ' ' || char.match(/[a-z]/i) === null) return char;
        return glitchChars[Math.floor(Math.random() * glitchChars.length)];
      }).join('');

      if (iter >= originalText.length) {
        clearInterval(glitchTimer);
        glitchTimer = null;
        el.innerHTML = originalHtml;
      }
      iter += 3;
    }, 40);
  }

  function resetTitle() {
    if (glitchTimer !== null) {
      clearInterval(glitchTimer);
      glitchTimer = null;
    }
    el.innerHTML = originalHtml;
  }

  el.addEventListener('mouseenter', glitch);
  el.addEventListener('mouseleave', resetTitle);
})();

/* ── Typing cursor on tagline ────────────── */
(function () {
  const el = document.querySelector('.tagline');
  if (!el) return;
  const text = el.innerHTML;
  el.innerHTML = '';
  let i = 0;
  const timer = setInterval(() => {
    el.innerHTML = text.slice(0, i) + '<span class="cursor">|</span>';
    i++;
    if (i > text.length) {
      el.innerHTML = text;
      clearInterval(timer);
    }
  }, 18);

  const style = document.createElement('style');
  style.textContent = `.cursor { color: var(--accent); animation: blink 1s step-end infinite; }
  @keyframes blink { 50% { opacity: 0; } }`;
  document.head.appendChild(style);
})();
