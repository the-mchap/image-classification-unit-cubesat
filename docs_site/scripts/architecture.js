(() => {
  'use strict';

  const SVG_NS = 'http://www.w3.org/2000/svg';

  // =========================================================
  // Flow particle animation system
  // =========================================================

  /**
   * @typedef {Object} FlowConfig
   * @property {string} pathId   - ID of the <path> element in SVG <defs>
   * @property {string} cssClass - CSS class applied to the particle (e.g. 'flow-purple')
   * @property {number} duration - Full cycle duration in milliseconds
   * @property {number} delay    - Start delay in milliseconds
   */

  /** @type {FlowConfig[]} Hardware diagram flow particles */
  const hwFlowConfig = [
    {
      pathId: 'path-mcu-earth',
      cssClass: 'flow-purple',
      duration: 2000,
      delay: 0,
    },
    {
      pathId: 'path-earth-mcu',
      cssClass: 'flow-purple',
      duration: 2400,
      delay: 600,
    },
    { pathId: 'path-mcu-sbc', cssClass: 'flow-pink', duration: 1600, delay: 0 },
    {
      pathId: 'path-sbc-mcu',
      cssClass: 'flow-pink',
      duration: 1800,
      delay: 400,
    },
    { pathId: 'path-cam-sbc', cssClass: 'flow-pink', duration: 1200, delay: 0 },
  ];

  /** @type {FlowConfig[]} State machine diagram flow particles (command arrows only) */
  const smFlowConfig = [
    {
      pathId: 'sm-idle-capture',
      cssClass: 'flow-purple',
      duration: 2000,
      delay: 0,
    },
    {
      pathId: 'sm-idle-downlink',
      cssClass: 'flow-purple',
      duration: 1800,
      delay: 300,
    },
    {
      pathId: 'sm-idle-purge',
      cssClass: 'flow-purple',
      duration: 2200,
      delay: 600,
    },
  ];

  /**
   * Creates animated particles that travel along SVG paths.
   * Particles are appended to the target SVG and driven by a
   * single requestAnimationFrame loop per diagram.
   *
   * @param {string}       svgSelector - CSS selector for the SVG element
   * @param {FlowConfig[]} config      - Array of flow definitions
   */
  function initFlowParticles(svgSelector, config) {
    const svg = document.querySelector(svgSelector);
    if (!svg) return;

    const particles = config
      .map((cfg) => {
        const path = document.getElementById(cfg.pathId);
        if (!path) return null;

        const circle = document.createElementNS(SVG_NS, 'circle');
        circle.classList.add('flow-particle', cfg.cssClass);
        svg.appendChild(circle);

        return {
          circle,
          path,
          totalLength: path.getTotalLength(),
          duration: cfg.duration,
          delay: cfg.delay,
          startTime: null,
        };
      })
      .filter(Boolean);

    /**
     * Single rAF loop driving all particles for this diagram.
     * @param {DOMHighResTimeStamp} timestamp
     */
    function animate(timestamp) {
      particles.forEach((p) => {
        if (!p.startTime) p.startTime = timestamp + p.delay;
        if (timestamp < p.startTime) return;

        const elapsed = (timestamp - p.startTime) % p.duration;
        const progress = elapsed / p.duration;
        const point = p.path.getPointAtLength(progress * p.totalLength);

        p.circle.setAttribute('cx', point.x);
        p.circle.setAttribute('cy', point.y);

        // Fade in at start, hold, fade out at end
        let opacity;
        if (progress < 0.1) {
          opacity = (progress / 0.1) * 0.9;
        } else if (progress > 0.8) {
          opacity = ((1 - progress) / 0.2) * 0.9;
        } else {
          opacity = 0.9;
        }
        p.circle.style.opacity = opacity;
      });

      requestAnimationFrame(animate);
    }

    requestAnimationFrame(animate);
  }

  // =========================================================
  // Info panel system (replaces floating tooltips)
  // =========================================================

  /**
   * @typedef {Object} InfoMeta
   * @property {string}   title - Display title
   * @property {string}   desc  - Description paragraph
   * @property {string[]} specs - Short spec tags shown below description
   */

  /** @type {Record<string, InfoMeta>} Hardware diagram component metadata */
  const hwComponents = {
    earth: {
      title: 'Ground Station',
      desc: 'Earth-side receiver. All telemetry and imagery uplinked from the ICU passes through the ground station for processing and distribution.',
      specs: ['UL/DL', 'Remote Ops'],
    },
    mcu: {
      title: 'MCU — Microcontroller Unit',
      desc: 'Ultra-low-power controller managing all uplink/downlink communications. Sole gateway between the ICU and Earth. Handles packet parsing, scheduling, and SBC power management.',
      specs: ['Ultra-low power', 'Always on', 'Comms gateway'],
    },
    sbc: {
      title: 'SBC — Raspberry Pi',
      desc: 'Single-board computer running on-board ML inference. Captures images and classifies them locally, only transmitting relevant results to save bandwidth and power.',
      specs: ['ML inference', 'Power on demand', 'Camera control'],
    },
    camera: {
      title: 'Camera Module',
      desc: 'Image sensor connected to the SBC via CSI. Activated only during capture cycles to minimize power draw.',
      specs: ['CSI interface', 'Duty-cycled'],
    },
  };

  /** @type {Record<string, InfoMeta>} State machine state metadata */
  const smStates = {
    idle: {
      title: 'IDLE',
      desc: 'Low-power standby. MCU listens for commands from Earth. SBC and camera are powered off to conserve energy.',
      specs: ['MCU only', 'Ultra-low power'],
    },
    capture: {
      title: 'CAPTURE',
      desc: 'SBC and camera power on. An image is captured and ML inference runs on-board. Only relevant images are queued for downlink.',
      specs: ['SBC + Camera', 'ML inference'],
    },
    downlink: {
      title: 'DOWNLINK',
      desc: 'MCU transmits queued telemetry and filtered imagery to Earth via uplink.',
      specs: ['MCU active', 'UL transmission'],
    },
    purge: {
      title: 'PURGE',
      desc: 'Stored images and data are cleared from local storage on command from Earth.',
      specs: ['MCU only', 'Storage cleanup'],
    },
  };

  /**
   * Wires up a fixed info panel for interactive SVG components.
   * Hover previews info (desktop), click/tap pins it.
   *
   * @param {string}                    containerSelector - CSS selector for the section wrapper
   * @param {string}                    itemSelector      - CSS selector for hoverable SVG elements
   * @param {string}                    dataAttr          - data-* attribute name holding the metadata key
   * @param {Record<string, InfoMeta>}  metadata          - Lookup table of panel content
   * @param {string}                    panelId           - ID of the info panel element
   */
  function initInfoPanel(
    containerSelector,
    itemSelector,
    dataAttr,
    metadata,
    panelId,
  ) {
    const container = document.querySelector(containerSelector);
    const panel = document.getElementById(panelId);
    if (!container || !panel) return;

    const titleEl = panel.querySelector('.info-title');
    const descEl = panel.querySelector('.info-desc');
    const specsEl = panel.querySelector('.info-specs');

    let pinnedKey = null;
    let pinnedEl = null;

    /**
     * Populates and reveals the info panel.
     * @param {string} key - Metadata key to display
     */
    function showInfo(key) {
      const data = metadata[key];
      if (!data) return;
      titleEl.textContent = data.title;
      descEl.textContent = data.desc;
      specsEl.innerHTML = data.specs.map((s) => `<span>${s}</span>`).join('');
      panel.classList.add('active');
    }

    /** Hides the info panel and shows the prompt. */
    function hideInfo() {
      panel.classList.remove('active');
    }

    container.querySelectorAll(itemSelector).forEach((el) => {
      const key = el.dataset[dataAttr];
      if (!metadata[key]) return;

      // Hover: preview info (desktop only, no-op on touch)
      el.addEventListener('mouseenter', () => {
        showInfo(key);
      });

      el.addEventListener('mouseleave', () => {
        if (pinnedKey) {
          showInfo(pinnedKey);
        } else {
          hideInfo();
        }
      });

      // Click/tap: pin selection
      el.addEventListener('click', () => {
        // Unpin if clicking the same component
        if (pinnedKey === key) {
          pinnedEl.classList.remove('active');
          pinnedKey = null;
          pinnedEl = null;
          hideInfo();
          return;
        }

        // Unpin previous
        if (pinnedEl) {
          pinnedEl.classList.remove('active');
        }

        // Pin new
        pinnedKey = key;
        pinnedEl = el;
        el.classList.add('active');
        showInfo(key);
      });
    });
  }

  // =========================================================
  // Init
  // =========================================================

  // Hardware diagram
  initFlowParticles('.hw-diagram', hwFlowConfig);
  initInfoPanel(
    '#hardware .diagram-container',
    '.component-box[data-component]',
    'component',
    hwComponents,
    'hw-info',
  );

  // State machine diagram
  initFlowParticles('.sm-diagram', smFlowConfig);
  initInfoPanel(
    '#operations .diagram-container',
    '.state-box[data-state]',
    'state',
    smStates,
    'sm-info',
  );
})();
