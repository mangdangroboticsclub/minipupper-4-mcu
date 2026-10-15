<script>
  /**
   * ActionIcon now renders a simple emoji based on the action id.
   * Built-in actions have predefined emojis; custom actions pass their emoji directly.
   */
  let { id, emoji } = $props();

  const builtinEmojis = {
    advance: "🚶",
    back: "🔙",
    left: "◀️",
    right: "▶️",
    turnL: "↺",
    turnR: "↻",
    stanford: "🐕",
    step: "🦶",
    testspeed: "⚡",
    jump: "🤸",
    jumpfwd: "🏃",
    flegR: "🦵",
    flegL: "🦵",
    blegR: "🦵",
    blegL: "🦵",
    heightup: "⬆️",
    heightdown: "⬇️",
    moveLF: "↗️",
    moveRF: "↖️",
    moveLB: "↘️",
    moveRB: "↙️",
    sit: "🪑",
    stretch: "🧘",
    twerk: "💃",
    roll: "🔄",
    pitch: "📐",
    balance: "⚖️",
    init: "🏁",
    none: "⏹️",
    frontkick: "🦶",
    wiggle: "🐕",
    wiggleL: "◀️",
    wiggleR: "▶️",
    buttshrug: "🍑",
    buttshrugL: "◀️",
    buttshrugR: "▶️",
    bowback: "🙇",
    bodycycle: "🔄",
    headellipse: "🔄",
  };

  const displayEmoji = $derived(emoji || builtinEmojis[id] || "⚙️");
</script>

<span class="action-emoji" role="img" aria-label={id}>{displayEmoji}</span>

<style>
  .action-emoji {
    font-size: 1.8rem;
    line-height: 1;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    /* Slight text-stroke for legibility on colored backgrounds */
    filter: drop-shadow(0 1px 1px rgba(0,0,0,0.15));
  }
</style>
