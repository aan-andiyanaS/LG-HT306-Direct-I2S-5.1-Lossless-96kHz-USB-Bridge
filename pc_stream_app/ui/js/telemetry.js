/**
 * HT306 Home Theater - Native Telemetry Processor & VU Meter Updates
 * Receives real-time telemetry from C++ DSP pipeline and updates UI meters, hardware badges, and states.
 */
(function () {
  "use strict";

  // --- GLOBAL STATE INITIALIZERS ---
  window.CHANNEL_NAMES = window.CHANNEL_NAMES || ["FL", "FR", "C", "LFE", "SL", "SR"];
  window.CHANNEL_LOWER = window.CHANNEL_LOWER || ["fl", "fr", "c", "lfe", "sl", "sr"];
  window.trimsDb = window.trimsDb || [0.0, 0.0, 1.5, 0.0, -1.0, -1.0];
  window.mutes = window.mutes || [false, false, false, false, false, false];
  window.solos = window.solos || [false, false, false, false, false, false];
  window.phases = window.phases || [false, false, false, false, false, false];
  window.routeState = window.routeState || { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5 };
  window.logBuffer = window.logBuffer || [];
  window.audioDriverState = window.audioDriverState || { installed: false, is_default: false, name: "", debug_override: false };
  window.isStreaming = window.isStreaming !== undefined ? window.isStreaming : true;
  window.isMuted = window.isMuted !== undefined ? window.isMuted : false;
  window.currentMode = window.currentMode || "desktop";
  window.currentProfile = window.currentProfile || "Movie";
  window.currentTab = window.currentTab || "dashboard";
  window.appUptimeSeconds = window.appUptimeSeconds || 0;

  // --- UTILITY FUNCTIONS ---
  function dbfsToPercent(db) {
    if (db <= -60) return 0;
    if (db >= 0) return 100;
    return ((db + 60) / 60) * 100;
  }
  window.dbfsToPercent = dbfsToPercent;

  function formatUptime(seconds) {
    const hrs = Math.floor(seconds / 3600).toString().padStart(2, "0");
    const mins = Math.floor((seconds % 3600) / 60).toString().padStart(2, "0");
    const secs = Math.floor(seconds % 60).toString().padStart(2, "0");
    return `${hrs}:${mins}:${secs}`;
  }
  window.formatUptime = formatUptime;

  function escapeHtml(str) {
    return String(str).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }
  window.escapeHtml = escapeHtml;

  // --- DOM REFERENCE & DIRTY-STATE CACHES ---
  let _dom = null;
  let _lastStreamBtnState = null;
  const _prev = {
    tab: null,
    linkKey: null,
    fmtBadge: null,
    rateKey: null,
    bitsText: null,
    chText: null,
    comPort: null,
    vidPid: null,
    uptime: null,
    fifo: null,
    throughput: null,
    underruns: null,
    clip: null,
    hb: null,
    pio: null,
    mux: null,
    dsp: null,
    vol: null,
    muteMaster: null,
    driverKey: null,
    profile: null
  };

  function getDom() {
    if (_dom) return _dom;
    const chCards = Array.from(document.querySelectorAll(".channel-card"));
    const mixerCards = [];
    const mixerVuBars = [];
    const mixerFaders = [];
    const mixerReadouts = [];
    const mixerMuteBtns = [];
    const mixerSoloBtns = [];
    const mixerPhaseBtns = [];

    for (let i = 0; i < window.CHANNEL_NAMES.length; i++) {
      const chUpper = window.CHANNEL_NAMES[i];
      const alt = chUpper === "LFE" ? "SW" : chUpper;
      const card = document.querySelector(`.channel-card[data-ch="${chUpper}"]`) ||
                   document.querySelector(`.channel-card[data-ch="${alt}"]`) ||
                   chCards[i] || null;
      mixerCards.push(card);
      mixerVuBars.push(card ? card.querySelector(".vu-bar") : null);
      mixerFaders.push(card ? card.querySelector(".ch-fader") : null);
      mixerReadouts.push(card ? card.querySelector(".ch-readout") : null);
      mixerMuteBtns.push(card ? card.querySelector(".btn-mute") : null);
      mixerSoloBtns.push(card ? card.querySelector(".btn-solo") : null);
      mixerPhaseBtns.push(card ? card.querySelector(".btn-phase") : null);
    }

    _dom = {
      streamBtn: document.getElementById("toggle-stream-btn"),
      headerDot: document.getElementById("header-status-dot"),
      headerText: document.getElementById("header-status-text"),
      headerProf: document.getElementById("header-profile-text"),
      dashDot: document.getElementById("dash-connected-dot"),
      dashText: document.getElementById("dash-connected-text"),
      asideFmt: document.getElementById("aside-format-badge"),
      asideCom: document.getElementById("aside-com-port"),
      dashRate: document.getElementById("dash-sample-rate"),
      dashBits: document.getElementById("dash-bit-depth"),
      dashCh: document.getElementById("dash-channels"),
      dashCom: document.getElementById("dash-com-port"),
      dashVidPid: document.getElementById("dash-vid-pid"),
      dashUptime: document.getElementById("dash-uptime"),
      dashFifo: document.getElementById("dash-fifo-depth"),
      dashBuf: document.getElementById("dash-buffer-fill"),
      dashLinkPct: document.getElementById("dash-link-pct"),
      dashLinkBar: document.getElementById("dash-link-bar"),
      dashBitrate: document.getElementById("dash-bitrate"),
      dashUnderruns: document.getElementById("dash-underruns"),
      dashDevName: document.getElementById("dash-device-name"),
      clipDot: document.getElementById("dash-clip-dot"),
      clipText: document.getElementById("dash-clip-text"),
      hbElem: document.getElementById("dash-heartbeat"),
      pioElem: document.getElementById("dash-pio-status"),
      muxElem: document.getElementById("dash-mux-status"),
      dspElem: document.getElementById("dash-dsp-status"),
      devSelect: document.getElementById("setting-device-select"),
      limiterToggle: document.getElementById("limiter-toggle"),
      lSlider: document.getElementById("lipsync-slider"),
      audioDot: document.getElementById("header-audio-dot"),
      audioText: document.getElementById("header-audio-text"),
      settingDriverStatus: document.getElementById("setting-driver-status"),
      btnInstallDriver: document.getElementById("btn-install-vbcable"),
      btnDebugAudio: document.getElementById("btn-debug-audio"),
      dashDebugAudioText: document.getElementById("dash-debug-audio-text"),
      dashDebugAudioIcon: document.getElementById("dash-debug-audio-icon"),
      settingDebugToggle: document.getElementById("setting-debug-audio-toggle"),
      settingDebugBadge: document.getElementById("setting-debug-audio-badge"),
      chBars: window.CHANNEL_LOWER.map(c => document.getElementById("meter-bar-" + c)),
      chPeaks: window.CHANNEL_LOWER.map(c => document.getElementById("peak-" + c)),
      chVals: window.CHANNEL_LOWER.map(c => document.getElementById("val-" + c)),
      mixerCards,
      mixerVuBars,
      mixerFaders,
      mixerReadouts,
      mixerMuteBtns,
      mixerSoloBtns,
      mixerPhaseBtns,
      masterSliders: Array.from(document.querySelectorAll("#master-slider")),
      masterVolNums: Array.from(document.querySelectorAll("#master-vol-num")),
      masterVolReadouts: Array.from(document.querySelectorAll("#master-vol-readout")),
      masterMeterBars: Array.from(document.querySelectorAll("#master-meter-bar")),
      muteLabels: Array.from(document.querySelectorAll("#mute-label")),
      muteBtns: Array.from(document.querySelectorAll("#master-mute-btn, #mute-btn"))
    };
    return _dom;
  }

  // --- UI STATUS HELPERS ---
  function updateStreamButtonUi(streaming) {
    const isOn = Boolean(streaming);
    if (_lastStreamBtnState === isOn) return;
    _lastStreamBtnState = isOn;

    const { streamBtn } = getDom();
    if (streamBtn) {
      if (isOn) {
        streamBtn.innerHTML = '<span class="material-symbols-outlined text-[18px]">stop_circle</span><span>Stop Stream</span>';
        streamBtn.className = "px-space-xl py-2.5 rounded-full bg-primary-container hover:bg-lg-red-hover active:bg-lg-red-pressed text-text-primary font-body-sm text-body-sm font-semibold shadow-lg shadow-primary-container/20 transition-all flex items-center justify-center gap-2 cursor-pointer";
      } else {
        streamBtn.innerHTML = '<span class="material-symbols-outlined text-[18px]">play_circle</span><span>Start Stream</span>';
        streamBtn.className = "px-space-xl py-2.5 rounded-full bg-status-ok hover:opacity-90 active:opacity-80 text-background font-body-sm text-body-sm font-semibold shadow-lg transition-all flex items-center justify-center gap-2 cursor-pointer";
      }
    }
  }
  window.updateStreamButtonUi = updateStreamButtonUi;

  function updateModeUi(mode) {
    const desktopBtn = document.getElementById("mode-desktop-btn");
    const passBtn = document.getElementById("mode-passthrough-btn");
    const bypassPill = document.getElementById("dsp-bypass-pill");

    if (mode === "bitperfect") {
      if (passBtn) passBtn.className = "px-space-md py-1 rounded-full font-code-sm text-code-sm font-medium transition-all bg-primary-container text-text-primary shadow-sm cursor-pointer";
      if (desktopBtn) desktopBtn.className = "px-space-md py-1 rounded-full font-code-sm text-code-sm text-text-secondary hover:text-text-primary transition-all cursor-pointer";
      if (bypassPill) {
        bypassPill.classList.remove("hidden");
        bypassPill.classList.add("flex");
      }
    } else {
      if (desktopBtn) desktopBtn.className = "px-space-md py-1 rounded-full font-code-sm text-code-sm font-medium transition-all bg-primary-container text-text-primary shadow-sm cursor-pointer";
      if (passBtn) passBtn.className = "px-space-md py-1 rounded-full font-code-sm text-code-sm text-text-secondary hover:text-text-primary transition-all cursor-pointer";
      if (bypassPill) {
        bypassPill.classList.add("hidden");
        bypassPill.classList.remove("flex");
      }
    }
  }
  window.updateModeUi = updateModeUi;

  // --- NATIVE TELEMETRY PROCESSOR ---
  function handleNativeTelemetry(raw) {
    let data = raw;
    if (typeof raw === "string") {
      try {
        data = JSON.parse(raw);
      } catch (e) {
        console.warn("[HT306 Telemetry] Parse error:", e);
        return;
      }
    }
    if (!data || typeof data !== "object") return;
    const dom = getDom();
    const curTab = window.currentTab || "dashboard";
    const tabChanged = (_prev.tab !== curTab);
    _prev.tab = curTab;

    // 1. Connection & Link Status
    if (data.state !== undefined || data.is_streaming !== undefined || data.connected !== undefined) {
      const explicitDisconnected = (data.state === "DISCONNECTED" || data.state === 0);
      const captureActive = (data.is_streaming === true) || (data.state === "STREAMING" || data.state === 3);
      let streaming = false;
      let connected = false;

      if (!explicitDisconnected) {
        if (data.state === "STREAMING" || data.state === 3) {
          streaming = true;
          connected = true;
        } else if (data.state === "READY" || data.state === "CONNECTED" || data.state === 2 || data.connected === true) {
          connected = true;
          streaming = (data.is_streaming === true);
        } else if (data.state === undefined && data.is_streaming === true) {
          streaming = true;
          connected = true;
        }
      }

      window.isStreaming = captureActive;
      updateStreamButtonUi(captureActive);

      const nextLinkKey = streaming ? "streaming" : (connected ? "connected" : "disconnected");
      if (_prev.linkKey !== nextLinkKey) {
        _prev.linkKey = nextLinkKey;
        if (streaming) {
          if (dom.headerDot) dom.headerDot.className = "w-2 h-2 rounded-full bg-status-ok";
          if (dom.headerText) {
            dom.headerText.textContent = "Streaming";
            dom.headerText.className = "font-code-sm text-code-sm text-status-ok font-medium";
          }
          if (dom.dashDot) dom.dashDot.className = "w-2.5 h-2.5 rounded-full bg-status-ok";
          if (dom.dashText) dom.dashText.textContent = "● STREAMING";
        } else if (connected) {
          if (dom.headerDot) dom.headerDot.className = "w-2 h-2 rounded-full bg-status-ok";
          if (dom.headerText) {
            dom.headerText.textContent = "Connected";
            dom.headerText.className = "font-code-sm text-code-sm text-status-ok font-medium";
          }
          if (dom.dashDot) dom.dashDot.className = "w-2.5 h-2.5 rounded-full bg-status-ok";
          if (dom.dashText) dom.dashText.textContent = "● CONNECTED";
        } else {
          if (dom.headerDot) dom.headerDot.className = "w-2 h-2 rounded-full bg-status-error";
          if (dom.headerText) {
            dom.headerText.textContent = "Disconnected";
            dom.headerText.className = "font-code-sm text-code-sm text-status-error font-medium";
          }
          if (dom.dashDot) dom.dashDot.className = "w-2.5 h-2.5 rounded-full bg-status-error";
          if (dom.dashText) dom.dashText.textContent = "● DISCONNECTED";
        }
      }
    }

    // 2. Format: 96kHz / 32b (preserve 0Hz as Standby instead of falsy-falling back to 96000)
    const rate = (data.format && typeof data.format.rate === "number") ? data.format.rate :
                 (data.telemetry && typeof data.telemetry.sample_rate === "number") ? data.telemetry.sample_rate :
                 (typeof data.sample_rate === "number" ? data.sample_rate : 96000);
    const bits = (data.format && typeof data.format.bits === "number") ? data.format.bits :
                 (data.telemetry && typeof data.telemetry.bit_depth === "number") ? data.telemetry.bit_depth :
                 (typeof data.bit_depth === "number" ? data.bit_depth : 32);
    const channels = (data.format && typeof data.format.channels === "number") ? data.format.channels :
                     (data.telemetry && typeof data.telemetry.channels === "number") ? data.telemetry.channels :
                     (typeof data.channels === "number" ? data.channels : 6);
    const inputRate = (data.telemetry && typeof data.telemetry.input_sample_rate === "number") ? data.telemetry.input_sample_rate :
                      (typeof data.input_sample_rate === "number" ? data.input_sample_rate : rate);
    const rateClamped = Boolean((data.telemetry && data.telemetry.rate_clamped) || data.rate_clamped);
    const sampleRateMode = data.sample_rate_mode ||
                           (data.telemetry && data.telemetry.sample_rate_mode) ||
                           (data.config && data.config.sample_rate_mode) || "96k";
    const capturePath = data.capture_path ||
                        (data.telemetry && data.telemetry.capture_path) || "";

    window.currentActiveSampleRate = rate > 0 ? rate : 96000;
    window.currentCapturePath = capturePath;

    const fmtBadgeText = rate > 0 ? `${Math.round(rate / 1000)}kHz / ${bits}b` : `Standby / ${bits}b`;
    if (_prev.fmtBadge !== fmtBadgeText) {
      _prev.fmtBadge = fmtBadgeText;
      if (dom.asideFmt) dom.asideFmt.textContent = fmtBadgeText;
    }

    const withinRateClickCooldown = (Date.now() - (window._lastSampleRateClickMs || 0)) < 400;
    if (!withinRateClickCooldown) {
      const rateKey = `${sampleRateMode}|${rate}|${rateClamped}|${inputRate}|${capturePath}`;
      if (_prev.rateKey !== rateKey) {
        _prev.rateKey = rateKey;
        if (typeof window.renderSampleRateUi === "function") {
          window.renderSampleRateUi(sampleRateMode, rate, rateClamped, inputRate, capturePath);
        } else if (dom.dashRate) {
          dom.dashRate.textContent = rate > 0 ? `${(rate / 1000).toFixed(1)} kHz` : "Standby";
        }
      }
    }

    const bitsText = `${bits}-bit Container`;
    if (_prev.bitsText !== bitsText) {
      _prev.bitsText = bitsText;
      if (dom.dashBits) dom.dashBits.textContent = bitsText;
    }

    const chText = channels === 6 ? "5.1 Surround" : `${channels} Channels`;
    if (_prev.chText !== chText) {
      _prev.chText = chText;
      if (dom.dashCh) dom.dashCh.textContent = chText;
    }

    // 3. Device & COM port
    const rawComPort = (data.device && data.device.com_port !== undefined) ? data.device.com_port :
                       ((data.hw_info && data.hw_info.com_port !== undefined) ? data.hw_info.com_port :
                       (data.com_port !== undefined ? data.com_port : undefined));
    if (rawComPort !== undefined) {
      const comDisplay = rawComPort ? String(rawComPort) : "--";
      if (_prev.comPort !== comDisplay) {
        _prev.comPort = comDisplay;
        if (dom.asideCom) dom.asideCom.textContent = comDisplay;
        if (dom.dashCom) dom.dashCom.textContent = comDisplay;
      }
    }

    const vid = (data.device && data.device.vid) || (data.hw_info && data.hw_info.vid) || data.vid;
    const pid = (data.device && data.device.pid) || (data.hw_info && data.hw_info.pid) || data.pid;
    if (vid !== undefined && pid !== undefined) {
      const vidHex = typeof vid === "number" ? "0x" + vid.toString(16).toUpperCase() : vid;
      const pidHex = typeof pid === "number" ? "0x" + pid.toString(16).toUpperCase() : pid;
      const vpText = `${vidHex} : ${pidHex}`;
      if (_prev.vidPid !== vpText) {
        _prev.vidPid = vpText;
        if (dom.dashVidPid) dom.dashVidPid.textContent = vpText;
      }
    }

    // 4. Uptime
    const uptimeSec = (data.hw_info && data.hw_info.uptime_sec !== undefined) ? data.hw_info.uptime_sec :
                      (data.uptime !== undefined ? data.uptime : null);
    if (uptimeSec !== null) {
      window.appUptimeSeconds = uptimeSec;
      if (_prev.uptime !== uptimeSec) {
        _prev.uptime = uptimeSec;
        if (dom.dashUptime) dom.dashUptime.textContent = formatUptime(uptimeSec);
      }
    }

    // 5. FIFO Depth & Buffer / Telemetry
    const fifoDepth = data.fifo_depth || (data.telemetry && data.telemetry.fifo_depth) || 4096;
    if (_prev.fifo !== fifoDepth && dom.dashFifo) {
      _prev.fifo = fifoDepth;
      dom.dashFifo.textContent = `FIFO DEPTH: ${fifoDepth} BYTES │ LATENCY OFFSET: 0.00ms`;
    }

    const bufferFill = (data.telemetry && data.telemetry.buffer_fill_pct !== undefined) ? data.telemetry.buffer_fill_pct :
                       (data.buffer_fill_pct !== undefined ? data.buffer_fill_pct : null);
    if (bufferFill !== null && dom.dashBuf) {
      dom.dashBuf.textContent = `${Math.round(bufferFill)}% Fill`;
    }

    const linkPct = (data.telemetry && data.telemetry.link_usage_pct !== undefined) ? data.telemetry.link_usage_pct :
                    (data.link_usage_pct !== undefined ? data.link_usage_pct : null);
    if (linkPct !== null) {
      if (dom.dashLinkPct) dom.dashLinkPct.textContent = `${linkPct}% Utilized`;
      if (dom.dashLinkBar) dom.dashLinkBar.style.width = `${linkPct}%`;
    }

    const throughput = (data.telemetry && data.telemetry.throughput_kbs !== undefined) ? data.telemetry.throughput_kbs :
                       (data.throughput_kbs !== undefined ? data.throughput_kbs : null);
    if (throughput !== null) {
      const roundedTp = Math.round(throughput);
      if (_prev.throughput !== roundedTp) {
        _prev.throughput = roundedTp;
        if (dom.dashBitrate) dom.dashBitrate.textContent = `${roundedTp} KB/s Current Rate`;
      }
    }

    const underruns = (data.telemetry && data.telemetry.underrun_count !== undefined) ? data.telemetry.underrun_count :
                      (data.stats && data.stats.dropped !== undefined) ? data.stats.dropped :
                      (data.underruns !== undefined ? data.underruns : null);
    if (underruns !== null && _prev.underruns !== underruns) {
      _prev.underruns = underruns;
      if (dom.dashUnderruns) dom.dashUnderruns.textContent = `${underruns} / hr`;
    }

    // 6. VU Meter levels: FL, FR, C, LFE, SL, SR (update only active view tab)
    let dBFS = null;
    let peakHolds = null;

    if (Array.isArray(data.dBFS)) {
      dBFS = data.dBFS;
      peakHolds = Array.isArray(data.peak_hold_db) ? data.peak_hold_db : data.dBFS;
    } else if (Array.isArray(data.meters)) {
      dBFS = [];
      peakHolds = [];
      for (let i = 0; i < data.meters.length; i++) {
        const m = data.meters[i];
        if (typeof m === "number") {
          dBFS.push(m);
          peakHolds.push(m);
        } else if (m && typeof m === "object") {
          dBFS.push(m.peak_db !== undefined ? m.peak_db : (m.db !== undefined ? m.db : -60.0));
          peakHolds.push(m.peak_hold_db !== undefined ? m.peak_hold_db : dBFS[i]);
        }
      }
    }

    if (dBFS && dBFS.length >= 6) {
      const updateDashVu = (curTab === "dashboard" || tabChanged);
      const updateMixerVu = (curTab === "mixer" || tabChanged);
      let maxDb = -96;
      let clipDetected = false;

      for (let i = 0; i < window.CHANNEL_NAMES.length; i++) {
        const db = dBFS[i];
        const hold = (peakHolds && peakHolds[i] !== undefined) ? peakHolds[i] : db;

        if (db > maxDb) maxDb = db;
        if (db >= -0.1) clipDetected = true;

        const pct = dbfsToPercent(db);
        const holdPct = dbfsToPercent(hold);

        if (updateDashVu) {
          const bar = dom.chBars[i];
          const peakMarker = dom.chPeaks[i];
          const valText = dom.chVals[i];
          if (bar) bar.style.width = pct.toFixed(1) + "%";
          if (peakMarker) peakMarker.style.left = holdPct.toFixed(1) + "%";
          if (valText) {
            valText.textContent = db > -80 ? db.toFixed(1) + " dB" : "-∞ dB";
            const warn = (db >= -3.0);
            if (valText._warn !== warn) {
              valText._warn = warn;
              valText.className = warn
                ? "w-16 min-w-[64px] text-right font-code-sm text-code-sm whitespace-nowrap flex-shrink-0 tabular-nums text-status-warn font-semibold"
                : "w-16 min-w-[64px] text-right font-code-sm text-code-sm whitespace-nowrap flex-shrink-0 tabular-nums text-text-primary font-medium";
            }
          }
        }

        if (updateMixerVu) {
          const mixerVu = dom.mixerVuBars[i];
          if (mixerVu && !window.mutes[i]) {
            mixerVu.style.height = pct.toFixed(1) + "%";
          }
        }
      }

      if (_prev.clip !== clipDetected && dom.clipDot && dom.clipText) {
        _prev.clip = clipDetected;
        if (clipDetected) {
          dom.clipDot.className = "w-2.5 h-2.5 rounded-full bg-status-error";
          dom.clipText.className = "font-code-sm text-code-sm text-status-error font-bold";
          dom.clipText.textContent = "CLIP DETECTED: 0 dBFS";
        } else {
          dom.clipDot.className = "w-2.5 h-2.5 rounded-full bg-status-ok";
          dom.clipText.className = "font-code-sm text-code-sm text-status-ok font-medium";
          dom.clipText.textContent = "CLIP: NONE (Clean Headroom)";
        }
      }

      if (updateMixerVu && dom.masterMeterBars.length > 0) {
        const masterPctStr = dbfsToPercent(maxDb).toFixed(1) + "%";
        for (let i = 0; i < dom.masterMeterBars.length; i++) {
          dom.masterMeterBars[i].style.width = masterPctStr;
        }
      }
    }

    // 7. Hardware flags & Heartbeat
    const heartbeat = data.heartbeat_text || (data.hw_info && data.hw_info.heartbeat_text);
    if (heartbeat && _prev.hb !== heartbeat) {
      _prev.hb = heartbeat;
      if (dom.hbElem) dom.hbElem.textContent = heartbeat;
    }

    const pioReady = data.pio_ready !== undefined ? data.pio_ready : (data.hw_info && data.hw_info.pio_ready);
    if (pioReady !== undefined && _prev.pio !== pioReady) {
      _prev.pio = pioReady;
      if (dom.pioElem) {
        dom.pioElem.textContent = pioReady ? "READY" : "OFFLINE";
        dom.pioElem.className = pioReady ? "font-code-lg text-code-lg text-tertiary font-semibold" : "font-code-lg text-code-lg text-status-error font-semibold";
      }
    }

    const muxHigh = data.mux_high !== undefined ? data.mux_high : (data.hw_info && data.hw_info.mux_high);
    if (muxHigh !== undefined && _prev.mux !== muxHigh) {
      _prev.mux = muxHigh;
      if (dom.muxElem) {
        dom.muxElem.textContent = muxHigh ? "HIGH" : "LOW";
        dom.muxElem.className = muxHigh ? "font-code-lg text-code-lg text-status-info font-semibold" : "font-code-lg text-code-lg text-status-warn font-semibold";
      }
    }

    const dspLocked = data.dsp_locked !== undefined ? data.dsp_locked : (data.hw_info && data.hw_info.dsp_locked);
    if (dspLocked !== undefined && _prev.dsp !== dspLocked) {
      _prev.dsp = dspLocked;
      if (dom.dspElem) {
        dom.dspElem.textContent = dspLocked ? "LOCKED" : "UNLOCKED";
        dom.dspElem.className = dspLocked ? "font-code-lg text-code-lg text-text-primary font-semibold" : "font-code-lg text-code-lg text-status-warn font-semibold";
      }
    }

    // 8. Format Mode (only if key present)
    if (data.format_mode !== undefined || (data.config && data.config.format_mode !== undefined)) {
      const fmode = data.format_mode !== undefined ? data.format_mode : data.config.format_mode;
      if (typeof window.setFormatModeUi === "function") {
        window.setFormatModeUi(fmode);
      }
    }

    // 9. Master Volume & Master Mute
    if (data.master_volume_db !== undefined || (data.config && data.config.master_volume_db !== undefined)) {
      const vol = data.master_volume_db !== undefined ? data.master_volume_db : data.config.master_volume_db;
      if (_prev.vol !== vol) {
        _prev.vol = vol;
        const volStr = (vol > 0 ? "+" : "") + vol.toFixed(1);
        for (let i = 0; i < dom.masterSliders.length; i++) {
          if (document.activeElement !== dom.masterSliders[i]) dom.masterSliders[i].value = vol;
        }
        for (let i = 0; i < dom.masterVolNums.length; i++) {
          dom.masterVolNums[i].textContent = volStr + " dB";
        }
        for (let i = 0; i < dom.masterVolReadouts.length; i++) {
          dom.masterVolReadouts[i].textContent = volStr;
        }
      }
    }

    if (data.mute_master !== undefined || (data.config && data.config.mute_master !== undefined)) {
      const mm = data.mute_master !== undefined ? data.mute_master : data.config.mute_master;
      if (_prev.muteMaster !== mm) {
        _prev.muteMaster = mm;
        window.isMuted = mm;
        for (let i = 0; i < dom.muteLabels.length; i++) {
          dom.muteLabels[i].textContent = window.isMuted ? "Unmute Master" : "Mute Master";
        }
        for (let i = 0; i < dom.muteBtns.length; i++) {
          const mMuteBtn = dom.muteBtns[i];
          if (window.isMuted) {
            mMuteBtn.classList.add("bg-status-error", "text-text-primary");
            mMuteBtn.classList.remove("bg-surface-2", "text-text-secondary");
          } else {
            mMuteBtn.classList.remove("bg-status-error", "text-text-primary");
            mMuteBtn.classList.add("bg-surface-2", "text-text-secondary");
          }
        }
      }
    }

    // 10. Solos and Phase inverts sync
    const solosArr = data.solos || (data.config && data.config.solos);
    if (Array.isArray(solosArr) && solosArr.length >= 6) {
      for (let i = 0; i < 6; i++) {
        if (window.solos[i] !== solosArr[i]) {
          window.solos[i] = solosArr[i];
          const bSolo = dom.mixerSoloBtns[i];
          if (bSolo) {
            if (window.solos[i]) {
              bSolo.classList.add("bg-status-warn", "text-background");
              bSolo.classList.remove("bg-surface-2", "text-text-secondary");
            } else {
              bSolo.classList.remove("bg-status-warn", "text-background");
              bSolo.classList.add("bg-surface-2", "text-text-secondary");
            }
          }
        }
      }
    }

    const phasesArr = data.phase_inverts || data.phases || (data.config && (data.config.phase_inverts || data.config.phases));
    if (Array.isArray(phasesArr) && phasesArr.length >= 6) {
      for (let i = 0; i < 6; i++) {
        if (window.phases[i] !== phasesArr[i]) {
          window.phases[i] = phasesArr[i];
          const bPhase = dom.mixerPhaseBtns[i];
          if (bPhase) {
            if (window.phases[i]) {
              bPhase.classList.add("bg-secondary-container", "text-text-primary");
              bPhase.classList.remove("bg-surface-2", "text-text-secondary");
              bPhase.textContent = "Ø";
            } else {
              bPhase.classList.remove("bg-secondary-container", "text-text-primary");
              bPhase.classList.add("bg-surface-2", "text-text-secondary");
              bPhase.textContent = "O";
            }
          }
        }
      }
    }

    // 11. Channel mutes sync
    const mutesArr = data.mutes || (data.config && data.config.mutes);
    if (Array.isArray(mutesArr) && mutesArr.length >= 6) {
      for (let i = 0; i < 6; i++) {
        if (window.mutes[i] !== mutesArr[i]) {
          window.mutes[i] = mutesArr[i];
          const btnMute = dom.mixerMuteBtns[i];
          const vu = dom.mixerVuBars[i];
          if (btnMute) {
            if (window.mutes[i]) {
              btnMute.classList.add("bg-status-error", "text-text-primary");
              btnMute.classList.remove("bg-surface-2", "text-text-secondary");
              if (vu) vu.style.opacity = "0.2";
            } else {
              btnMute.classList.remove("bg-status-error", "text-text-primary");
              btnMute.classList.add("bg-surface-2", "text-text-secondary");
              if (vu) vu.style.opacity = "1";
            }
          }
        }
      }
    }

    // 12. Routing map sync
    const rmap = data.routing_map || (data.config && data.config.routing_map);
    if (Array.isArray(rmap) && rmap.length >= 6) {
      let rChanged = false;
      for (let i = 0; i < 6; i++) {
        if (window.routeState[i] !== rmap[i]) {
          window.routeState[i] = rmap[i];
          rChanged = true;
        }
      }
      if (rChanged && typeof window.renderMatrix === "function") {
        window.renderMatrix();
      }
    }

    // 13. Audio Capture Devices sync (only when devices[] sent)
    if (Array.isArray(data.devices) && data.devices.length > 0) {
      const devSelect = dom.devSelect;
      if (devSelect) {
        let needsUpdate = devSelect.options.length !== data.devices.length;
        if (!needsUpdate) {
          for (let i = 0; i < data.devices.length; i++) {
            const expectedText = data.devices[i].name + (data.devices[i].is_default ? " (Default)" : "");
            if (devSelect.options[i].text !== expectedText) {
              needsUpdate = true;
              break;
            }
          }
        }
        if (needsUpdate) {
          devSelect.innerHTML = "";
          data.devices.forEach((dev) => {
            const opt = document.createElement("option");
            opt.value = dev.index;
            opt.textContent = dev.name + (dev.is_default ? " (Default)" : "");
            devSelect.appendChild(opt);
          });
        }
        const activeIdx = data.active_device_index !== undefined ? data.active_device_index :
                          (data.config && data.config.active_device_index !== undefined ? data.config.active_device_index : 0);
        if (String(devSelect.value) !== String(activeIdx)) {
          devSelect.value = activeIdx;
        }
      }

      if (dom.dashDevName) {
        const activeIdx = data.active_device_index !== undefined ? data.active_device_index : 0;
        const activeDev = data.devices.find(d => d.index === activeIdx) || data.devices[0];
        if (activeDev && dom.dashDevName.textContent !== activeDev.name) {
          dom.dashDevName.textContent = activeDev.name;
        }
      }
    }

    // 14. Limiter & LipSync sync
    if (data.limiter_enabled !== undefined || (data.config && data.config.limiter_enabled !== undefined)) {
      const lim = data.limiter_enabled !== undefined ? data.limiter_enabled : data.config.limiter_enabled;
      if (dom.limiterToggle && document.activeElement !== dom.limiterToggle && dom.limiterToggle.checked !== lim) {
        dom.limiterToggle.checked = lim;
      }
    }

    if (data.lip_sync_delay_ms !== undefined || (data.config && data.config.lip_sync_delay_ms !== undefined)) {
      const delay = data.lip_sync_delay_ms !== undefined ? data.lip_sync_delay_ms : data.config.lip_sync_delay_ms;
      if (dom.lSlider && document.activeElement !== dom.lSlider && parseInt(dom.lSlider.value, 10) !== delay) {
        if (typeof window.updateDelayUi === "function") {
          window.updateDelayUi(delay);
        }
      }
    }

    // 15. Config / Profiles sync
    if (data.profile || (data.config && data.config.profile) || (data.settings && data.settings.active_profile)) {
      const prof = data.profile || (data.config && data.config.profile) || data.settings.active_profile;
      if (window.currentProfile !== prof) {
        window.currentProfile = prof;
        if (dom.headerProf) dom.headerProf.textContent = prof;
      }
    }

    if (data.mode || (data.config && data.config.mode)) {
      const m = data.mode || data.config.mode;
      if (m !== window.currentMode) {
        window.currentMode = m;
        updateModeUi(window.currentMode);
      }
    }

    if (data.trims_db || (data.config && data.config.trims_db)) {
      const t = data.trims_db || data.config.trims_db;
      if (Array.isArray(t)) {
        window.trimsDb = t;
        for (let idx = 0; idx < 6 && idx < t.length; idx++) {
          const num = parseFloat(t[idx]);
          if (!Number.isFinite(num)) continue;
          const fader = dom.mixerFaders[idx];
          const readout = dom.mixerReadouts[idx];
          if (fader && fader !== document.activeElement && parseFloat(fader.value) !== num) {
            fader.value = num;
          }
          const label = (num > 0 ? "+" : "") + num.toFixed(1) + " dB";
          if (readout && readout.textContent !== label) {
            readout.textContent = label;
          }
        }
      }
    }

    // 16. Logs stream handling (only re-render terminal DOM when Log tab is visible)
    if (Array.isArray(data.logs)) {
      const nextLen = data.logs.length;
      const lastNext = nextLen > 0 ? String(data.logs[nextLen - 1]) : "";
      const lastCur = window.logBuffer.length > 0 ? String(window.logBuffer[window.logBuffer.length - 1]) : "";
      if (nextLen !== window.logBuffer.length || lastNext !== lastCur) {
        window.logBuffer = data.logs.map(l => typeof l === "string" ? l : (l.message || JSON.stringify(l)));
        if (window.currentTab === "log" && typeof window.renderLogs === "function") {
          window.renderLogs();
        }
      }
    } else if (data.log) {
      const line = typeof data.log === "string" ? data.log : (data.log.message || JSON.stringify(data.log));
      window.logBuffer.push(line);
      if (window.logBuffer.length > 500) window.logBuffer.shift();
      if (window.currentTab === "log" && typeof window.renderLogs === "function") {
        window.renderLogs();
      }
    }

    // 17. Audio Driver & Windows Taskbar Status sync (dirty-checked to prevent innerHTML thrashing)
    if (data.audio_driver) {
      window.audioDriverState = data.audio_driver;
      const drvKey = JSON.stringify(data.audio_driver);
      if (_prev.driverKey !== drvKey) {
        _prev.driverKey = drvKey;
        const isDebugOverride = !!window.audioDriverState.debug_override;
        if (dom.settingDebugToggle && document.activeElement !== dom.settingDebugToggle) {
          dom.settingDebugToggle.checked = isDebugOverride;
        }
        if (dom.settingDebugBadge) {
          dom.settingDebugBadge.textContent = isDebugOverride ? "ON (Tanpa RP2350)" : "OFF";
          dom.settingDebugBadge.className = isDebugOverride
            ? "font-code-sm text-[10px] text-status-warn font-semibold px-1.5 py-0.2 rounded bg-status-warn/10 border border-status-warn/40"
            : "font-code-sm text-[10px] text-icon-gray px-1.5 py-0.2 rounded bg-surface border border-border";
        }
        if (dom.btnDebugAudio && dom.dashDebugAudioText) {
          if (isDebugOverride) {
            dom.btnDebugAudio.className = "flex items-center gap-1.5 px-space-sm py-1 rounded bg-status-warn/15 hover:bg-status-warn/25 border border-status-warn/50 text-status-warn font-code-sm text-code-sm font-semibold transition-colors cursor-pointer";
            dom.dashDebugAudioText.textContent = "Debug Audio: ON (Tanpa RP2350)";
            if (dom.dashDebugAudioIcon) dom.dashDebugAudioIcon.className = "material-symbols-outlined text-[15px] text-status-warn";
          } else {
            dom.btnDebugAudio.className = "flex items-center gap-1.5 px-space-sm py-1 rounded bg-surface-2 hover:bg-surface-variant border border-border text-text-secondary hover:text-text-primary font-code-sm text-code-sm transition-colors cursor-pointer";
            dom.dashDebugAudioText.textContent = "Debug Audio: OFF";
            if (dom.dashDebugAudioIcon) dom.dashDebugAudioIcon.className = "material-symbols-outlined text-[15px] text-icon-gray";
          }
        }

        if (window.audioDriverState.installed) {
          const isVisible = window.audioDriverState.visible !== false;
          if (!isVisible) {
            if (dom.audioDot) dom.audioDot.className = "w-2 h-2 rounded-full bg-status-info";
            if (dom.audioText) {
              dom.audioText.textContent = "Taskbar: Standby (RP2350 Off)";
              dom.audioText.className = "font-code-sm text-code-sm text-status-info font-medium";
            }
            if (dom.settingDriverStatus) {
              dom.settingDriverStatus.innerHTML = `<span class="text-status-info font-medium">Terpasang — Siaga (Muncul Saat RP2350 Terhubung)</span>`;
            }
            if (dom.btnInstallDriver) {
              dom.btnInstallDriver.innerHTML = `<span class="material-symbols-outlined text-[16px]">visibility</span><span>Tampilkan Manual (Debug)</span>`;
              dom.btnInstallDriver.className = "px-space-md py-1.5 bg-surface border border-border hover:bg-surface-variant text-text-primary rounded-lg font-caption text-caption font-semibold flex items-center gap-1.5 transition-colors cursor-pointer";
            }
          } else {
            if (window.audioDriverState.is_default) {
              if (dom.audioDot) dom.audioDot.className = isDebugOverride ? "w-2 h-2 rounded-full bg-status-warn" : "w-2 h-2 rounded-full bg-status-ok";
              if (dom.audioText) {
                dom.audioText.textContent = "Taskbar: " + (window.audioDriverState.name || "LG HT306") + (isDebugOverride ? " [DEBUG]" : "");
                dom.audioText.className = isDebugOverride
                  ? "font-code-sm text-code-sm text-status-warn font-medium"
                  : "font-code-sm text-code-sm text-status-ok font-medium";
              }
            } else {
              if (dom.audioDot) dom.audioDot.className = "w-2 h-2 rounded-full bg-status-info";
              if (dom.audioText) {
                dom.audioText.textContent = "Taskbar: Standby" + (isDebugOverride ? " [DEBUG]" : "");
                dom.audioText.className = "font-code-sm text-code-sm text-status-info font-medium";
              }
            }
            if (dom.settingDriverStatus) {
              dom.settingDriverStatus.innerHTML = `<span class="${isDebugOverride ? 'text-status-warn' : 'text-status-ok'} font-medium">Aktif di Taskbar (${escapeHtml(window.audioDriverState.name || 'LG HT306')}${isDebugOverride ? ' — Mode Debug' : ''})</span>`;
            }
            if (dom.btnInstallDriver) {
              const hasCableName = (window.audioDriverState.name && window.audioDriverState.name.includes("CABLE"));
              if (hasCableName) {
                dom.btnInstallDriver.innerHTML = `<span class="material-symbols-outlined text-[16px]">draw</span><span>Sinkronkan Nama ke LG HT306</span>`;
                dom.btnInstallDriver.className = "px-space-md py-1.5 bg-primary-container hover:bg-lg-red-hover text-text-primary rounded-lg font-caption text-caption font-semibold flex items-center gap-1.5 transition-colors cursor-pointer";
              } else {
                dom.btnInstallDriver.innerHTML = `<span class="material-symbols-outlined text-[16px]">sync_alt</span><span>${window.audioDriverState.is_default ? 'Kembalikan Speaker PC' : 'Jadikan Default Taskbar'}</span>`;
                dom.btnInstallDriver.className = "px-space-md py-1.5 bg-surface border border-border hover:bg-surface-variant text-text-primary rounded-lg font-caption text-caption font-semibold flex items-center gap-1.5 transition-colors cursor-pointer";
              }
            }
          }
        } else {
          if (dom.audioDot) dom.audioDot.className = "w-2 h-2 rounded-full bg-status-warn";
          if (dom.audioText) {
            dom.audioText.textContent = "+ Pasang Driver Taskbar";
            dom.audioText.className = "font-code-sm text-code-sm text-status-warn font-medium";
          }
          if (dom.settingDriverStatus) {
            dom.settingDriverStatus.innerHTML = `<span class="text-status-warn font-medium">Belum Terpasang</span>`;
          }
          if (dom.btnInstallDriver) {
            dom.btnInstallDriver.innerHTML = `<span class="material-symbols-outlined text-[16px]">download</span><span>Pasang Driver Audio</span>`;
            dom.btnInstallDriver.className = "px-space-md py-1.5 bg-primary-container hover:bg-lg-red-hover text-text-primary rounded-lg font-caption text-caption font-semibold flex items-center gap-1.5 transition-colors cursor-pointer";
          }
        }
      }
    }
  }

  // --- EXPOSE TELEMETRY CONTRACTS ---
  window.updateTelemetry = handleNativeTelemetry;
  window.handleNativeTelemetry = handleNativeTelemetry;
  window.onNativeTelemetry = handleNativeTelemetry;

  // --- BROWSER FALLBACK POLLING ---
  function pollStatus() {
    if (window.isNativeWebView2) return;
    fetch("/api/status")
      .then((res) => {
        if (!res.ok) throw new Error("HTTP " + res.status);
        return res.json();
      })
      .then((data) => {
        handleNativeTelemetry(data);
      })
      .catch(() => {
        const headerDot = document.getElementById("header-status-dot");
        const headerText = document.getElementById("header-status-text");
        if (headerDot && headerText) {
          headerDot.className = "w-2 h-2 rounded-full bg-status-error";
          headerText.innerText = "Disconnected";
          headerText.className = "font-code-sm text-code-sm text-status-error font-medium";
        }
      });
  }

  function pollMeters() {
    if (window.isNativeWebView2) return;
    fetch("/api/meters")
      .then((res) => {
        if (!res.ok) throw new Error("HTTP " + res.status);
        return res.json();
      })
      .then((data) => {
        handleNativeTelemetry(data);
      })
      .catch(() => {});
  }

  if (!window.isNativeWebView2 && window.location.protocol.startsWith("http")) {
    setInterval(pollStatus, 500);
    pollStatus();
    setInterval(pollMeters, 60);
  }
})();
