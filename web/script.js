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

const copyButton = document.querySelector('.copy-button');
const commandBlock = document.getElementById('quick-start-code');

if (copyButton && commandBlock) {
  let resetTimer;
  copyButton.addEventListener('click', async () => {
    const commands = commandBlock.textContent
      .split('\n')
      .filter((line) => line.trim() && !line.trim().startsWith('#'))
      .join('\n');

    try {
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(commands);
      } else {
        const fallback = document.createElement('textarea');
        fallback.value = commands;
        fallback.style.position = 'fixed';
        fallback.style.opacity = '0';
        document.body.append(fallback);
        fallback.select();
        const copied = document.execCommand('copy');
        fallback.remove();
        if (!copied) throw new Error('Clipboard unavailable');
      }
      copyButton.textContent = 'Copied!';
    } catch {
      copyButton.textContent = 'Copy failed';
    }

    clearTimeout(resetTimer);
    resetTimer = setTimeout(() => {
      copyButton.textContent = 'Copy commands';
    }, 2200);
  });
}
