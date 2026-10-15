<script>
  import { colors } from "./colors.js";

  let { onBack } = $props();

  const YELLOW = colors.mpx.primary;

  let selectedLeg = $state(null); // null | 'fl' | 'fr' | 'rl' | 'rr'
  let loading = $state(true);

  const legs = [
    { id: 'fl', label: 'FL' },
    { id: 'fr', label: 'FR' },
    { id: 'rl', label: 'RL' },
    { id: 'rr', label: 'RR' },
  ];

  // Servo index base for each leg: FL=1, FR=4, RL=7, RR=10
  const legServoBase = { fl: 0, fr: 3, rl: 6, rr: 9 };

  let calValues = $state({
    fl: { shoulder: 0, hip: 0, knee: 0 },
    fr: { shoulder: 0, hip: 0, knee: 0 },
    rl: { shoulder: 0, hip: 0, knee: 0 },
    rr: { shoulder: 0, hip: 0, knee: 0 },
  });

  const calProps = [
    { id: 'shoulder', label: 'Shoulder', min: -50, max: 50, step: 1 },
    { id: 'hip', label: 'Hip', min: -50, max: 50, step: 1 },
    { id: 'knee', label: 'Knee', min: -50, max: 50, step: 1 },
  ];

  let currentLegValues = $derived(selectedLeg ? calValues[selectedLeg] : null);

  // ── API: Load offsets from robot ────────────────────────────
  async function loadOffsets() {
    loading = true;
    try {
      const res = await fetch("/v1/robot/status");
      if (res.ok) {
        const data = await res.json();
        if (data.offsets && data.offsets.length === 12) {
          calValues = {
            fl: { shoulder: data.offsets[0] || 0, hip: data.offsets[1] || 0, knee: data.offsets[2] || 0 },
            fr: { shoulder: data.offsets[3] || 0, hip: data.offsets[4] || 0, knee: data.offsets[5] || 0 },
            rl: { shoulder: data.offsets[6] || 0, hip: data.offsets[7] || 0, knee: data.offsets[8] || 0 },
            rr: { shoulder: data.offsets[9] || 0, hip: data.offsets[10] || 0, knee: data.offsets[11] || 0 },
          };
        }
      }
    } catch { /* use defaults */ }
    loading = false;
  }

  // ── API: Update a single servo offset ───────────────────────
  async function updateOffset(servoIndex, offset) {
    try {
      await fetch("/v1/robot/calibrate", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ servo: servoIndex, offset }),
      });
    } catch { /* ignore */ }
  }

  // ── API: Reset all offsets ──────────────────────────────────
  async function resetOffsets() {
    try {
      await fetch("/v1/robot/calibrate/reset", { method: "POST" });
      calValues = {
        fl: { shoulder: 0, hip: 0, knee: 0 },
        fr: { shoulder: 0, hip: 0, knee: 0 },
        rl: { shoulder: 0, hip: 0, knee: 0 },
        rr: { shoulder: 0, hip: 0, knee: 0 },
      };
    } catch { /* ignore */ }
  }

  // ── Slider drag state ──
  let draggingProp = $state(null);
  let dragBarEl = $state(null);

  function selectLeg(id) {
    selectedLeg = id;
  }

  function resetAll() {
    if (!selectedLeg) return;
    calValues[selectedLeg] = { shoulder: 0, hip: 0, knee: 0 };
    // Also send reset to robot for this leg's servos
    const base = legServoBase[selectedLeg];
    for (let i = 0; i < 3; i++) {
      updateOffset(base + i + 1, 0);
    }
  }

  function adjustProp(id, delta) {
    if (!selectedLeg) return;
    const prop = calProps.find(p => p.id === id);
    if (!prop) return;
    const cur = calValues[selectedLeg][id];
    const newVal = Math.max(prop.min, Math.min(prop.max, cur + delta));
    calValues[selectedLeg][id] = newVal;
    // Send to robot
    const base = legServoBase[selectedLeg];
    const jointIdx = calProps.findIndex(p => p.id === id);
    updateOffset(base + jointIdx + 1, newVal);
  }

  function sliderValueFromPointer(prop, clientX) {
    if (!dragBarEl) return 0;
    const rect = dragBarEl.getBoundingClientRect();
    const pct = (clientX - rect.left) / rect.width;
    const range = prop.max - prop.min;
    return Math.round(prop.min + pct * range);
  }

  function startDrag(e, propId) {
    if (!selectedLeg) return;
    const prop = calProps.find(p => p.id === propId);
    if (!prop) return;
    const handle = e.currentTarget;
    handle.setPointerCapture(e.pointerId);
    dragBarEl = handle.parentElement;
    draggingProp = propId;
    const val = sliderValueFromPointer(prop, e.clientX);
    calValues[selectedLeg][propId] = Math.max(prop.min, Math.min(prop.max, val));
  }

  function onDragMove(e) {
    if (!draggingProp || !selectedLeg) return;
    const prop = calProps.find(p => p.id === draggingProp);
    if (!prop) return;
    const val = sliderValueFromPointer(prop, e.clientX);
    calValues[selectedLeg][draggingProp] = Math.max(prop.min, Math.min(prop.max, val));
  }

  function endDrag() {
    if (draggingProp && selectedLeg) {
      // Send final value to robot
      const base = legServoBase[selectedLeg];
      const jointIdx = calProps.findIndex(p => p.id === draggingProp);
      updateOffset(base + jointIdx + 1, calValues[selectedLeg][draggingProp]);
    }
    draggingProp = null;
    dragBarEl = null;
  }

  function goBack() {
    if (selectedLeg) {
      selectedLeg = null;
    } else if (onBack) {
      onBack();
    }
  }

  // ── Load offsets from robot on mount ────────────────────────
  $effect(() => { loadOffsets(); });
</script>

<div class="calibration-root" style="--yellow: {YELLOW}">
  <!-- ═══ Header ═══ -->
  <header class="cal-header">
    <button class="cal-back-btn" onclick={goBack} aria-label="Go back">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="19" y1="12" x2="5" y2="12"/>
        <polyline points="12 19 5 12 12 5"/>
      </svg>
    </button>
    <h2 class="cal-title">Calibration</h2>
  </header>

  <!-- ═══ SVG Diagram Area ═══ -->
  <div class="cal-diagram-area">
    <svg width="379" height="425" viewBox="0 0 379 425" fill="none" xmlns="http://www.w3.org/2000/svg" class="cal-svg">
      <path d="M208.162 233.447C211.7 230.21 215.213 227.151 218.5 224.5C217.458 218.271 223.13 217.646 224 218.5C227.007 218.873 228.36 227.332 220 227.5C204.403 243.923 196.165 253.639 182.5 272L182.374 271.874C181.922 273.212 180.409 277.864 180.089 281.139C179.84 283.69 181 282.5 180.089 287.703C179.286 292.286 176.073 293.289 173.008 292.955L173 293H166.5C161.151 290.769 160.625 287.935 159.5 283C159.338 274.569 163.726 270.564 177.44 266.659C177.916 264.099 190.796 249.803 204.401 236.946L175.357 198.205L177.901 197.753L178.131 198.054L179.535 197.5L208.162 233.447Z" fill="#615F5D"/>
      <path d="M180.883 302V295.455H185.217V296.596H182.267V298.155H184.93V299.296H182.267V302H180.883ZM186.148 302V295.455H187.532V300.859H190.338V302H186.148Z" fill="black"/>
      <path d="M221.063 288V281.455H223.645C224.139 281.455 224.561 281.543 224.911 281.72C225.262 281.895 225.53 282.143 225.713 282.464C225.898 282.784 225.991 283.16 225.991 283.593C225.991 284.027 225.897 284.401 225.71 284.714C225.522 285.026 225.25 285.264 224.895 285.43C224.541 285.597 224.113 285.68 223.61 285.68H221.881V284.567H223.386C223.65 284.567 223.87 284.531 224.044 284.459C224.219 284.386 224.349 284.278 224.434 284.133C224.522 283.988 224.565 283.808 224.565 283.593C224.565 283.375 224.522 283.192 224.434 283.043C224.349 282.894 224.218 282.781 224.041 282.704C223.867 282.625 223.646 282.586 223.38 282.586H222.446V288H221.063ZM224.597 285.021L226.224 288H224.696L223.105 285.021H224.597ZM226.969 288V281.455H228.353V286.859H231.159V288H226.969Z" fill="black"/>
      <path d="M290.663 297V290.455H293.245C293.739 290.455 294.161 290.543 294.511 290.72C294.862 290.895 295.13 291.143 295.313 291.464C295.498 291.784 295.591 292.16 295.591 292.593C295.591 293.027 295.497 293.401 295.31 293.714C295.122 294.026 294.85 294.264 294.495 294.43C294.141 294.597 293.713 294.68 293.21 294.68H291.481V293.567H292.986C293.25 293.567 293.47 293.531 293.645 293.459C293.819 293.386 293.949 293.278 294.034 293.133C294.122 292.988 294.165 292.808 294.165 292.593C294.165 292.375 294.122 292.192 294.034 292.043C293.949 291.894 293.818 291.781 293.641 291.704C293.467 291.625 293.246 291.586 292.98 291.586H292.047V297H290.663ZM294.197 294.021L295.824 297H294.297L292.705 294.021H294.197ZM296.569 297V290.455H299.151C299.646 290.455 300.067 290.543 300.417 290.72C300.768 290.895 301.036 291.143 301.219 291.464C301.404 291.784 301.497 292.16 301.497 292.593C301.497 293.027 301.403 293.401 301.216 293.714C301.028 294.026 300.757 294.264 300.401 294.43C300.047 294.597 299.619 294.68 299.116 294.68H297.387V293.567H298.892C299.157 293.567 299.376 293.531 299.551 293.459C299.725 293.386 299.855 293.278 299.941 293.133C300.028 292.988 300.072 292.808 300.072 292.593C300.072 292.375 300.028 292.192 299.941 292.043C299.855 291.894 299.724 291.781 299.548 291.704C299.373 291.625 299.152 291.586 298.886 291.586H297.953V297H296.569ZM300.104 294.021L301.73 297H300.203L298.611 294.021H300.104Z" fill="black"/>
      <path d="M121.983 298V291.455H126.317V292.596H123.367V294.155H126.03V295.296H123.367V298H121.983ZM127.248 298V291.455H129.83C130.325 291.455 130.747 291.543 131.096 291.72C131.448 291.895 131.715 292.143 131.898 292.464C132.084 292.784 132.176 293.16 132.176 293.593C132.176 294.027 132.083 294.401 131.895 294.714C131.708 295.026 131.436 295.264 131.08 295.43C130.726 295.597 130.298 295.68 129.795 295.68H128.066V294.567H129.572C129.836 294.567 130.055 294.531 130.23 294.459C130.405 294.386 130.535 294.278 130.62 294.133C130.707 293.988 130.751 293.808 130.751 293.593C130.751 293.375 130.707 293.192 130.62 293.043C130.535 292.894 130.404 292.781 130.227 292.704C130.052 292.625 129.832 292.586 129.565 292.586H128.632V298H127.248ZM130.783 295.021L132.41 298H130.882L129.29 295.021H130.783Z" fill="black"/>
      <g filter="url(#filter0_d_0_1)">
        <path d="M287.5 217L295 214.5L310 232L302.5 240L287.5 217Z" fill="#111111"/>
      </g>
      <g filter="url(#filter1_d_0_1)">
        <path d="M285 218L275.5 217V203.5L289 208L285 218Z" fill="#030303"/>
      </g>
      <g filter="url(#filter2_d_0_1)">
        <path d="M147 216L90.5 211.5L121.5 203L154.5 200L275.5 211.5V226L147 216Z" fill="#2F2F2E"/>
      </g>
      <g filter="url(#filter3_d_0_1)">
        <path d="M180.5 188L87.5 161L123.5 136.5L213 152.5L180.5 188Z" fill="#545355"/>
      </g>
      <g filter="url(#filter4_d_0_1)">
        <path d="M141.5 242L121 214L137.5 212L155 237.5C155.307 242.43 153.905 243.392 150 243.5C150 243.5 146.979 245.49 145 245C143.252 244.568 141.5 242 141.5 242Z" fill="black"/>
      </g>
      <g filter="url(#filter5_d_0_1)">
        <path d="M234.819 243.084L223 224H238.818L246.983 236.971C247.224 242.019 244.946 242.419 241.88 242.529C241.88 242.529 240.371 245 238.818 245C236.777 245 234.819 243.084 234.819 243.084Z" fill="black"/>
      </g>
      <g filter="url(#filter6_d_0_1)">
        <path d="M274.5 220L238 217L274.5 228V220Z" fill="#040404"/>
        <path d="M147 219L126.5 221.5C133.221 223.453 146.016 224.839 147 224.5C147.801 224.709 225.026 227.667 274.5 228L147 219Z" fill="#595959"/>
        <path d="M274.5 228C223.715 225.651 201.756 225.324 154 220.5L186 213.5L238 217L274.5 228Z" fill="#242424"/>
        <path d="M147.5 219.5V211L203.5 214.5C188.634 219.072 180.779 220.433 165 220.5C158.141 220.529 153.865 220.943 147.5 219.5Z" fill="#040404"/>
        <path d="M126.5 221.5V214L147.5 211V219.5L126.5 221.5Z" fill="#2C2C2C"/>
      </g>
      <g filter="url(#filter7_d_0_1)">
        <path d="M90.5 204.5L85 206H90.5V204.5Z" fill="#2D2D2C"/>
        <path d="M90.5 204.5L85 206H90.5V204.5Z" stroke="#2D2D2C" stroke-linejoin="round"/>
      </g>
      <g filter="url(#filter8_d_0_1)">
        <path d="M82.5 206L89.5 204.5V169C85.4113 170.225 84.0767 172.085 83 177C81.9232 181.915 82.5 206 82.5 206Z" fill="#B69D1F"/>
        <path d="M82.5 206L89.5 204.5V169C85.4113 170.225 84.0767 172.085 83 177C81.9232 181.915 82.5 206 82.5 206Z" stroke="#B69D1F" stroke-linejoin="round"/>
      </g>
      <g filter="url(#filter9_d_0_1)">
        <path d="M120.5 212L89.5 211.5L121 205.5H133L148.5 209L120.5 212Z" fill="#2D2D2C"/>
      </g>
      <g filter="url(#filter10_d_0_1)">
        <path d="M148.5 208.5L276.5 217V172.5L133.268 153.898L132.5 205.5L148.5 208.5Z" fill="#0F100F"/>
        <path d="M148.5 208.5L276.5 217V172.5L133.268 153.898L132.5 205.5L148.5 208.5Z" stroke="#0F100F" stroke-linejoin="round"/>
      </g>
      <g filter="url(#filter11_d_0_1)">
        <path d="M166.5 293H173L178 266.5C163.845 270.456 159.336 274.455 159.5 283C160.625 287.935 161.151 290.769 166.5 293Z" fill="#2D2C2A"/>
        <path d="M164 281C164.128 274.656 165.885 271.519 177.5 268C168.204 272.092 165.883 275.094 164 281Z" fill="#5B5A58"/>
        <path d="M164.444 279.624C166.798 273.195 169.626 270.35 178.133 267L182.5 271.5C182.5 271.5 180.466 277.277 180.089 281.139C179.839 283.69 181 282.5 180.089 287.703C179.177 292.907 175.158 293.497 171.777 292.753C170.75 292.493 161.948 286.262 164.444 279.624Z" fill="#242422"/>
        <path d="M204.634 237.258L175.357 198.205L177.901 197.753L206.671 235.295L204.634 237.258Z" fill="#2D2C2B"/>
        <path d="M207 237L177 198.5L179.535 197.5L209 234.5L207 237Z" fill="#464543"/>
        <path d="M177.5 267C175.9 265.543 200.578 238.955 218.5 224.5C217.458 218.271 223.13 217.646 224 218.5C227.007 218.873 228.36 227.332 220 227.5C204.403 243.923 196.165 253.639 182.5 272L177.5 267Z" fill="#615F5D"/>
      </g>
      <g filter="url(#filter12_d_0_1)">
        <path d="M103.456 277.452C105.873 271.274 108.778 268.54 117.515 265.321L122 269.645C122 269.645 119.911 275.197 119.524 278.908C119.268 281.36 120.459 280.216 119.524 285.216C118.588 290.216 114.46 290.784 110.988 290.068C109.933 289.818 100.892 283.831 103.456 277.452Z" fill="#242422"/>
        <path d="M105.813 290.321H112.136L117 265.321C103.231 269.052 98.8444 272.826 99.0042 280.887C100.098 285.542 100.61 288.216 105.813 290.321Z" fill="#2D2C2A"/>
        <path d="M103 279.321C103.133 272.977 104.955 269.839 117 266.321C107.36 270.413 104.953 273.414 103 279.321Z" fill="#5B5A58"/>
        <path d="M117.074 265.767C115.471 264.441 140.202 240.227 158.161 227.063C157.117 221.39 162.801 220.82 163.673 221.599C166.686 221.938 168.042 229.642 159.665 229.795C144.034 244.751 135.779 253.599 122.085 270.321L117.074 265.767Z" fill="#615F5D"/>
      </g>
      <g filter="url(#filter13_d_0_1)">
        <path d="M205.325 274.515C207.129 269.481 209.297 267.254 215.817 264.63L219.164 268.154C219.164 268.154 217.605 272.677 217.316 275.701C217.125 277.699 218.015 276.767 217.316 280.841C216.618 284.915 213.537 285.377 210.946 284.795C210.159 284.591 203.412 279.712 205.325 274.515Z" fill="#242422"/>
        <path d="M207.085 285H211.803L215.433 264.63C205.157 267.671 201.884 270.745 202.003 277.313C202.82 281.107 203.202 283.285 207.085 285Z" fill="#2D2C2A"/>
        <path d="M204.985 276.037C205.084 270.869 206.444 268.312 215.433 265.445C208.239 268.78 206.443 271.225 204.985 276.037Z" fill="#5B5A58"/>
        <path d="M215.488 264.994C214.291 263.913 232.747 244.184 246.15 233.458C245.371 228.836 249.613 228.372 250.264 229.006C252.512 229.283 253.524 235.56 247.272 235.684C235.608 247.87 229.447 255.08 219.228 268.704L215.488 264.994Z" fill="#615F5D"/>
      </g>
      <g filter="url(#filter14_d_0_1)">
        <path d="M275 278.5C274.65 272.258 281.777 266.248 285 266L289.5 271C289.5 271 287.292 271.985 287 275.5C286.708 279.016 288.812 281.489 287 284.5C285.631 286.776 284.143 288.24 281.5 288.5C276.865 288.956 275.26 283.151 275 278.5Z" fill="#242422"/>
        <path d="M271.5 274C271.515 272.699 277.115 265.646 285 265.5L280.5 288.5C276.141 288.698 273.991 287.921 271.5 282.5C270.036 280.079 271.294 274.357 271.5 274Z" fill="#2D2C2A"/>
        <path d="M275 279C275.085 273.144 276.756 269.248 284.5 266C278.675 269.852 276.591 273.615 275 279Z" fill="#5B5A58"/>
        <path d="M303.354 241.8L279.224 206.117L281.143 205.595L304.815 239.875L303.354 241.8Z" fill="#2D2C2B"/>
        <path d="M305.326 241.404L280.142 206.538L282.074 205.505L306.771 238.988L305.326 241.404Z" fill="#464543"/>
        <path d="M284.5 266C283.256 264.701 300.993 240.34 314.921 227.444C314.111 221.887 318.519 221.329 319.195 222.092C321.532 222.424 322.584 229.971 316.087 230.121C303.965 244.772 300.12 254.62 289.5 271L284.5 266Z" fill="#615F5D"/>
      </g>
      <g filter="url(#filter15_d_0_1)">
        <path d="M284.612 207.182C276.604 207.456 272.511 205.387 270.02 201.846C270.02 201.846 269.296 176.079 276.653 172.056C284.009 168.032 295.586 176.463 297.878 181.393C300.171 186.322 319.324 234.082 318.662 234.305C320.402 241.066 315.124 244.976 309.818 239.196C304.511 233.416 291.245 213.407 291.245 213.407C289.016 210.253 287.567 208.863 284.612 207.182Z" fill="#D7BB0C"/>
      </g>
      <g filter="url(#filter16_d_0_1)">
        <path d="M283 168H266L263.5 172.5L264 204H277.5C280.634 203.725 282.5 203 283 200C283.5 197 283 168 283 168Z" fill="#DABD0D"/>
      </g>
      <g filter="url(#filter17_d_0_1)">
        <path d="M264 204H262L261.5 174.5C261.121 171.995 261.479 170.577 263.5 168H266.5C264.711 169.896 264.066 171.243 264 174.5L264 204Z" fill="#E7CA38"/>
      </g>
      <g filter="url(#filter18_d_0_1)">
        <path d="M251.5 206V176C251.722 169.261 254.77 167.682 265 168C263.201 169.165 262.133 170.48 262.5 176V204.18C256.391 204.298 254.402 204.587 251.5 206Z" fill="#CCB115"/>
      </g>
      <path d="M179 157C178.224 157.506 177.835 157.857 177.5 159L212.5 213.5L242.5 215C244.019 214.725 244.471 214.093 244.5 212L244 167.5C243.407 166.105 242.591 165.836 241 165.5L179 157Z" fill="#D1B50D"/>
      <g filter="url(#filter19_i_0_1)">
        <rect x="228" y="176.907" width="3" height="29.0835" rx="1.5" transform="rotate(-17.599 228 176.907)" fill="#292A2C"/>
      </g>
      <g filter="url(#filter20_i_0_1)">
        <rect x="220" y="175.907" width="3" height="30.2486" rx="1.5" transform="rotate(-17.599 220 175.907)" fill="#292A2C"/>
      </g>
      <g filter="url(#filter21_i_0_1)">
        <rect x="212" y="175.907" width="3" height="30.3702" rx="1.5" transform="rotate(-17.599 212 175.907)" fill="#292A2C"/>
      </g>
      <g filter="url(#filter22_i_0_1)">
        <rect x="204" y="174.907" width="3" height="30.861" rx="1.5" transform="rotate(-17.599 204 174.907)" fill="#292A2C"/>
      </g>
      <path d="M182 199.5C172.945 199.808 168.317 197.482 165.5 193.5C165.5 193.5 164.682 164.525 173 160C181.318 155.475 194.408 164.957 197 170.5C199.592 176.043 221.249 229.749 220.5 230C222.467 237.603 216.5 242 210.5 235.5C204.5 229 189.5 206.5 189.5 206.5C186.98 202.954 185.341 201.39 182 199.5Z" fill="#D7BB0C"/>
      <g filter="url(#filter23_d_0_1)">
        <path d="M175.5 154L152 151.5C149.673 153.815 149.058 155.48 149 159V194.5H170C172.958 193.427 174.227 192.123 175.5 188V154Z" fill="#DABD0C"/>
      </g>
      <path d="M152.5 151.5H150.5C148.074 152.646 147.03 154.249 147 159.5V194.5H149V159.5C149.591 155.055 150.449 153.46 152.5 151.5Z" fill="#E7CB37"/>
      <path d="M138 162C139.083 154.344 141.909 152.131 150.5 151.5C148.177 153.166 147.379 154.868 147 159.5V194.5C142.345 194.583 140.453 195.07 138 196.5V162Z" fill="#C2A91C"/>
      <path d="M138.5 159H133L132.5 198.5L138.5 197V159Z" fill="#121212"/>
      <path d="M127 205.5V152L133 153.5V205.5H127Z" fill="#1B1C1D"/>
      <path d="M121 206L121.5 152H127V206H121Z" fill="#424346"/>
      <path d="M89.5 211.5V162L121.5 152V206L89.5 211.5Z" fill="#C6AB1D"/>
      <path d="M93.5 203V167L116 160.5V199.5L93.5 203Z" fill="#1D1E20"/>
      <path d="M246.5 162L224.5 136.5C222.549 134.709 221.355 134.028 219 133.5L128 118V149L244 165.5C246.711 165.916 247.871 164.86 246.5 162Z" fill="#D9BC0D"/>
      <path d="M86.5 135C85.8396 136.329 86.5 161 86.5 161L125.5 149V118C125.5 118 91.1603 131.856 89.5 132.5C87.8396 133.144 87.1603 133.671 86.5 135Z" fill="#C3A91F"/>
      <path d="M128.5 149H125.5V118C126.672 117.514 127.328 117.461 128.5 118V149Z" fill="#ECD15D"/>
      <rect x="88" y="139" width="3" height="18" rx="1.5" fill="#424346"/>
      <rect x="94" y="137" width="3" height="18" rx="1.5" fill="#424346"/>
      <rect x="100" y="135" width="3" height="18" rx="1.5" fill="#424346"/>
      <rect x="107" y="132" width="3" height="18" rx="1.5" fill="#424346"/>
      <rect x="113" y="130" width="3" height="18" rx="1.5" fill="#424346"/>
      <rect x="120" y="128" width="3" height="18" rx="1.5" fill="#424346"/>
      
      <defs>
        {#each ['filter0_d_0_1','filter1_d_0_1','filter2_d_0_1','filter3_d_0_1','filter4_d_0_1','filter5_d_0_1','filter6_d_0_1','filter7_d_0_1','filter8_d_0_1','filter9_d_0_1','filter10_d_0_1','filter11_d_0_1','filter12_d_0_1','filter13_d_0_1','filter14_d_0_1','filter15_d_0_1','filter16_d_0_1','filter17_d_0_1','filter18_d_0_1','filter19_i_0_1','filter20_i_0_1','filter21_i_0_1','filter22_i_0_1','filter23_d_0_1'] as f}
          <filter id={f} x="-50%" y="-50%" width="200%" height="200%" filterUnits="userSpaceOnUse" color-interpolation-filters="sRGB">
            <feFlood flood-opacity="0" result="BackgroundImageFix"/>
            <feColorMatrix in="SourceAlpha" type="matrix" values="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 127 0" result="hardAlpha"/>
            <feOffset dy="2"/>
            <feGaussianBlur stdDeviation="1.5"/>
            <feComposite in2="hardAlpha" operator="out"/>
            <feColorMatrix type="matrix" values="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0.15 0"/>
            <feBlend mode="normal" in2="BackgroundImageFix" result="effect1_dropShadow_0_1"/>
            <feBlend mode="normal" in="SourceGraphic" in2="effect1_dropShadow_0_1" result="shape"/>
          </filter>
        {/each}
      </defs>
    </svg>
  </div>

  <!-- ═══ Leg Selection Buttons ═══ -->
  <div class="cal-leg-selector">
    {#each legs as leg}
      <button
        class="cal-leg-btn"
        class:selected={selectedLeg === leg.id}
        onclick={() => selectLeg(leg.id)}
        aria-label="Select {leg.label} leg"
      >
        {leg.label}
      </button>
    {/each}
  </div>

  <!-- ═══ Controls Panel (shown when leg selected) ═══ -->
  {#if selectedLeg}
    <div class="cal-controls">
      <div class="cal-controls-header">
        <span class="cal-leg-label">Leg: {legs.find(l => l.id === selectedLeg)?.label}</span>
        <button class="cal-controls-reset-btn" onclick={resetAll} aria-label="Reset all values">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
            <polyline points="23 4 23 10 17 10"/>
            <path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>
          </svg>
        </button>
      </div>

      {#each calProps as prop}
        {@const val = currentLegValues[prop.id]}
        <div class="cal-control-row">
          <div class="cal-control-row-header">
            <span class="cal-control-label">{prop.label}</span>
            <span class="cal-control-value">{val}</span>
          </div>
          <div class="cal-slider-row">
            <button
              class="cal-step-btn"
              onclick={() => adjustProp(prop.id, -prop.step)}
              aria-label="Decrease {prop.label}"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                <line x1="5" y1="12" x2="19" y2="12"/>
              </svg>
            </button>
            <div
              class="cal-slider-bar"
            >
              <div
                class="cal-slider-fill"
                style="left: {val < 0 ? 50 + val: 50}%; width: {Math.abs(val)}%"
              ></div>
              <div
                class="cal-slider-handle"
                class:cal-slider-dragging={draggingProp === prop.id}
                style="left: {50 + val}%"
                role="slider"
                tabindex="0"
                aria-label={prop.label}
                aria-valuemin={prop.min}
                aria-valuemax={prop.max}
                aria-valuenow={val}
                onpointerdown={(e) => startDrag(e, prop.id)}
                onpointermove={onDragMove}
                onpointerup={endDrag}
                onpointercancel={endDrag}
              ></div>
            </div>
            <button
              class="cal-step-btn"
              onclick={() => adjustProp(prop.id, prop.step)}
              aria-label="Increase {prop.label}"
            >
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">
                <line x1="12" y1="5" x2="12" y2="19"/>
                <line x1="5" y1="12" x2="19" y2="12"/>
              </svg>
            </button>
          </div>
        </div>
      {/each}

    </div>
  {:else}
    <!-- ═══ Tap hint (start state) ═══ -->
    <p class="cal-hint">Select a leg to calibrate</p>
  {/if}
</div>

<style>
  .calibration-root {
    flex: 1;
    background: var(--surface-1);
    display: flex;
    flex-direction: column;
    overflow-y: auto;
  }

  .cal-header {
    display: flex;
    align-items: center;
    gap: 8px;
    height: 52px;
    padding: 0 12px;
    flex-shrink: 0;
    background: var(--bg);
    border-bottom: 1px solid var(--line);
  }

  .cal-back-btn {
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: var(--surface-2);
    border-radius: 50%;
    cursor: pointer;
    color: var(--text-1);
    flex-shrink: 0;
  }
  .cal-back-btn:hover {
    background: var(--surface-3);
  }

  .cal-title {
    font-size: 1.05rem;
    font-weight: 800;
    color: var(--text-1);
    margin: 0;
  }

  .cal-controls-reset-btn {
    width: 28px;
    height: 28px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: none;
    background: var(--surface-2);
    border-radius: 50%;
    cursor: pointer;
    color: var(--text-2);
    flex-shrink: 0;
    transition: background 0.15s;
  }
  .cal-controls-reset-btn:hover {
    background: var(--surface-3);
  }

  .cal-diagram-area {
    display: flex;
    justify-content: center;
    padding: 4px 0 0;
    flex-shrink: 0;
  }

  .cal-svg {
    max-width: 75%;
    height: auto;
  }

  /* ── Leg Selection Buttons ── */
  .cal-leg-selector {
    display: flex;
    justify-content: center;
    gap: 14px;
    padding: 4px 14px 6px;
    flex-shrink: 0;
  }

  .cal-leg-btn {
    width: 44px;
    height: 44px;
    border-radius: 50%;
    border: 2.5px solid var(--line);
    background: var(--surface-2);
    color: var(--text-2);
    font-size: 0.8rem;
    font-weight: 700;
    cursor: pointer;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: all 0.15s;
  }
  .cal-leg-btn:hover {
    border-color: var(--line-strong);
    background: var(--surface-2);
  }
  .cal-leg-btn.selected {
    border-color: var(--yellow, var(--accent));
    background: var(--accent);
    color: var(--accent-ink);
    box-shadow: 0 2px 8px rgba(255, 230, 5, 0.4);
  }

  /* ── Controls Panel ── */
  .cal-controls {
    padding: 6px 18px 12px;
    display: flex;
    flex-direction: column;
    gap: 8px;
    flex-shrink: 0;
  }

  .cal-controls-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .cal-leg-label {
    font-size: 0.8rem;
    font-weight: 700;
    color: var(--text-1);
    background: var(--yellow, var(--accent));
    padding: 2px 14px;
    border-radius: 999px;
  }

  .cal-control-row {
    display: flex;
    flex-direction: column;
    gap: 3px;
  }

  .cal-control-row-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .cal-control-label {
    font-size: 0.75rem;
    font-weight: 600;
    color: var(--text-1);
  }

  .cal-control-value {
    font-size: 0.75rem;
    font-weight: 700;
    color: var(--text-1);
    font-variant-numeric: tabular-nums;
  }

  .cal-slider-row {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .cal-step-btn {
    width: 28px;
    height: 28px;
    border-radius: 8px;
    border: 1.5px solid var(--line);
    background: var(--surface-1);
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    color: var(--text-2);
    flex-shrink: 0;
    transition: background 0.15s, border-color 0.15s;
  }
  .cal-step-btn:hover {
    background: var(--surface-2);
    border-color: var(--line-strong);
  }
  .cal-step-btn:active {
    background: var(--surface-3);
  }

  .cal-slider-bar {
    position: relative;
    flex: 1;
    height: 4px;
    border-radius: 3px;
    background: var(--surface-2);
    padding: 10px 0;
  }

  .cal-slider-fill {
    position: absolute;
    top: 10px;
    height: 4px;
    border-radius: 4px;
    background: var(--yellow, var(--accent));
    pointer-events: none;
  }

  .cal-slider-handle {
    position: absolute;
    top: 50%;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: var(--yellow, var(--accent));
    border: 3px solid var(--line);
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
    transform: translate(-50%, -50%);
    z-index: 2;
    cursor: grab;
    touch-action: none;
    transition: box-shadow 0.15s;
  }
  .cal-slider-handle:hover {
    box-shadow: 0 2px 8px rgba(0,0,0,0.35);
  }
  .cal-slider-handle.cal-slider-dragging {
    cursor: grabbing;
    box-shadow: 0 2px 10px rgba(0,0,0,0.4);
  }

  .cal-hint {
    text-align: center;
    font-size: 0.75rem;
    font-weight: 500;
    color: var(--text-3);
    padding: 10px;
    margin: 0;
    flex-shrink: 0;
  }
</style>
