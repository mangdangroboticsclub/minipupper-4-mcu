<script>
  /** The eyes from the robot's face, as an icon. `open` shows the two tall
   *  eyes (locked / idle); closed shows the blink. Always the accent colour. */
  let { open = true, size = 40, class: klass = "" } = $props();
</script>

{#if open}
  <svg class={klass} width={size} height={size * 124 / 181} viewBox="0 0 181 124" fill="none" aria-hidden="true">
    <rect x="0.5" y="0.5" width="71" height="119" rx="17.5" fill="currentColor"/>
    <rect x="109.5" y="0.5" width="71" height="119" rx="17.5" fill="currentColor"/>
  </svg>
{:else}
  <svg class={klass} width={size} height={size * 51 / 156} viewBox="0 0 156 51" fill="none" aria-hidden="true">
    <rect width="64" height="42" rx="16" fill="currentColor"/>
    <rect x="92" width="64" height="34" rx="16" fill="currentColor"/>
  </svg>
{/if}
