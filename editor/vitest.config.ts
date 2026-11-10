import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'jsdom',
    globals: false,
    // VM from the tests does not write the database next to the repository programs (autosave - your test)
    env: { VISH_NO_AUTOSAVE: '1' },
  },
});
