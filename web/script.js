const menuButton = document.querySelector('.menu-toggle');
const navigation = document.querySelector('.site-nav');

if (menuButton && navigation) {
  const closeMenu = () => {
    menuButton.setAttribute('aria-expanded', 'false');
    menuButton.setAttribute('aria-label', 'Open menu');
    navigation.classList.remove('is-open');
  };

  menuButton.addEventListener('click', () => {
    const isOpen = menuButton.getAttribute('aria-expanded') === 'true';
    menuButton.setAttribute('aria-expanded', String(!isOpen));
    menuButton.setAttribute('aria-label', isOpen ? 'Open menu' : 'Close menu');
    navigation.classList.toggle('is-open', !isOpen);
  });

  navigation.addEventListener('click', (event) => {
    if (event.target.closest('a')) closeMenu();
  });

  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') closeMenu();
  });

  window.matchMedia('(min-width: 761px)').addEventListener('change', closeMenu);
}

async function copyText(text) {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return;
    } catch {
      // The fallback also works when the page is opened directly from disk.
    }
  }

  const fallback = document.createElement('textarea');
  fallback.value = text;
  fallback.style.position = 'fixed';
  fallback.style.opacity = '0';
  document.body.append(fallback);
  fallback.select();
  try {
    if (!document.execCommand('copy')) throw new Error('Clipboard unavailable');
  } finally {
    fallback.remove();
  }
}

for (const copyButton of document.querySelectorAll('[data-copy-target]')) {
  const commandBlock = document.getElementById(copyButton.dataset.copyTarget);
  if (!commandBlock) continue;

  const originalLabel = copyButton.textContent;
  let resetTimer;
  copyButton.addEventListener('click', async () => {
    const commands = commandBlock.textContent
      .split('\n')
      .filter((line) => line.trim() && !line.trim().startsWith('#'))
      .join('\n');

    try {
      await copyText(commands);
      copyButton.textContent = 'Copied!';
    } catch {
      copyButton.textContent = 'Copy failed';
    }

    clearTimeout(resetTimer);
    resetTimer = setTimeout(() => {
      copyButton.textContent = originalLabel;
    }, 2200);
  });
}
