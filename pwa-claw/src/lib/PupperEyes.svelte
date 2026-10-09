<script>
  /**
   * PupperEyes — the MangDang eyes as a reusable, animated mark.
   *
   * awake=true  → open eyes that blink every few seconds
   * awake=false → sleepy (closed) eyes
   * mood="happy" adds a little bounce, mood="think" drifts the pupils.
   */
  let {
    awake = true,
    size = "100%",   // any CSS size for width; height follows the aspect ratio
    color = "#000",  // eye colour
    accent = "#FFE605",
    mood = "idle",   // idle | happy | think
    blink = true,
  } = $props();
</script>

{#if awake}
  <svg
    class="eyes open"
    class:blink
    class:happy={mood === "happy"}
    class:think={mood === "think"}
    style="width: {size}"
    viewBox="0 0 181 124"
    fill="none"
    aria-hidden="true"
  >
    <g class="eye">
      <rect x="0.5" y="0.5" width="71" height="119" rx="17.5" fill={color} />
      <path class="lid" d="M16.5 110.5C11.9092 114.387 11.23 117.413 9 123H68C64.9899 117.521 63.1846 114.639 58.5 110.5C55.1314 107.523 53.1879 105.634 49 104C42.2693 101.374 34.4575 102.053 27.5 104C22.2014 105.483 20.6992 106.945 16.5 110.5Z" fill={accent} />
    </g>
    <g class="eye eye-r">
      <rect x="109.5" y="0.5" width="71" height="119" rx="17.5" fill={color} />
      <path class="lid" d="M122.5 110.224C117.909 114.111 117.23 117.137 115 122.724H174C170.99 117.245 169.185 114.363 164.5 110.224C161.131 107.247 159.188 105.358 155 103.724C148.269 101.098 140.457 101.777 133.5 103.724C128.201 105.207 126.699 106.669 122.5 110.224Z" fill={accent} />
    </g>
  </svg>
{:else}
  <svg class="eyes closed" style="width: {size}" viewBox="0 0 156 51" fill="none" aria-hidden="true">
    <rect width="64" height="42" rx="16" fill={color} />
    <path d="M1.5 49V21.5L62.5 25V45C30.626 39.5779 18.4342 39.6692 1.5 49Z" fill={color} stroke={color} stroke-width="3" stroke-linecap="round" stroke-linejoin="round" />
    <rect x="92" width="64" height="34" rx="16" fill={color} />
    <path d="M93.5 39.5V16H154.5V35C124 31.5 108 33.5 93.5 39.5Z" fill={color} stroke={color} stroke-width="3" stroke-linecap="round" stroke-linejoin="round" />
  </svg>
{/if}

<style>
  .eyes { display: block; height: auto; overflow: visible; }

  .eye {
    transform-box: fill-box;
    transform-origin: 50% 55%;
  }
  .blink .eye { animation: blink 5.4s ease-in-out infinite; }
  .blink .eye-r { animation-delay: 0.04s; }
  @keyframes blink {
    0%, 92%, 100% { transform: scaleY(1); }
    95% { transform: scaleY(0.08); }
  }

  .happy .eye { animation: blink 5.4s ease-in-out infinite, bounce 1.6s ease-in-out infinite; }
  @keyframes bounce {
    0%, 100% { translate: 0 0; }
    50% { translate: 0 -4px; }
  }

  .think .eye { animation: blink 5.4s ease-in-out infinite, drift 3.2s ease-in-out infinite; }
  @keyframes drift {
    0%, 100% { translate: 0 0; }
    35% { translate: 4px 0; }
    70% { translate: -4px 0; }
  }
</style>
