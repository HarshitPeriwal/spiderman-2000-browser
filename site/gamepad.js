// Gamepad support for Spider-Man (2000) PC under Boxedwine.
//
// Boxedwine does not pass browser gamepads to Wine, so the game's DirectInput
// controller path never sees one. This bridge presses the game's own default
// keyboard keys instead, so input reaches the game through its keyboard path,
// unchanged.
//
// This matches the original controller behaviour: the game itself reduces a
// joystick to digital directions (PCINPUT_GetControllerDirections, threshold
// 250 of 1000 on each axis), so a stick past 25% becomes an arrow key here.
//
// Default keyboard table, from the exe's gDefaultKeyboardMappings:
//   directions  Up/Down/Left/Right arrows (DIK 0xC8/0xD0/0xCB/0xCD)
//   actions     Numpad 2, 6, 4, 8, 7, 3, 9 (DIK 0x50/0x4D/0x4B/0x48/0x47/0x51/0x49)
//   Enter, Tab  (DIK 0x1C/0x0F)
// Face buttons follow the numpad diamond: bottom = Num2, right = Num6,
// left = Num4, top = Num8. If you change the keyboard configuration in the
// game's options, change PAD_MAP to match (or set localStorage "spidey.padmap").

(function () {
  const KEYS = {
    ArrowUp: { key: "ArrowUp", keyCode: 38, location: 0 },
    ArrowDown: { key: "ArrowDown", keyCode: 40, location: 0 },
    ArrowLeft: { key: "ArrowLeft", keyCode: 37, location: 0 },
    ArrowRight: { key: "ArrowRight", keyCode: 39, location: 0 },
    Numpad2: { key: "2", keyCode: 98, location: 3 },
    Numpad3: { key: "3", keyCode: 99, location: 3 },
    Numpad4: { key: "4", keyCode: 100, location: 3 },
    Numpad6: { key: "6", keyCode: 102, location: 3 },
    Numpad7: { key: "7", keyCode: 103, location: 3 },
    Numpad8: { key: "8", keyCode: 104, location: 3 },
    Numpad9: { key: "9", keyCode: 105, location: 3 },
    Enter: { key: "Enter", keyCode: 13, location: 0 },
    Tab: { key: "Tab", keyCode: 9, location: 0 },
    Escape: { key: "Escape", keyCode: 27, location: 0 },
  };

  // W3C standard gamepad button index -> KeyboardEvent.code
  const DEFAULT_MAP = {
    0: "Numpad2",   // A / Cross        (bottom)
    1: "Numpad6",   // B / Circle       (right)
    2: "Numpad4",   // X / Square       (left)
    3: "Numpad8",   // Y / Triangle     (top)
    4: "Numpad7",   // LB / L1
    5: "Numpad9",   // RB / R1
    7: "Numpad3",   // RT / R2
    8: "Tab",       // Back / Select
    9: "Enter",     // Start
    12: "ArrowUp", 13: "ArrowDown", 14: "ArrowLeft", 15: "ArrowRight",   // d-pad
  };
  const AXIS_THRESHOLD = 0.25;   // the game's own 250/1000

  let PAD_MAP = DEFAULT_MAP;
  try {
    const saved = JSON.parse(localStorage.getItem("spidey.padmap") || "null");
    if (saved && typeof saved === "object") PAD_MAP = saved;
  } catch {}

  const down = new Set();   // codes currently held by the bridge

  function send(type, code) {
    const k = KEYS[code];
    if (!k) return;
    const ev = new KeyboardEvent(type, { key: k.key, code, location: k.location, bubbles: true, cancelable: true });
    // Emscripten's SDL reads keyCode/which, which the constructor leaves at 0.
    Object.defineProperty(ev, "keyCode", { get: () => k.keyCode });
    Object.defineProperty(ev, "which", { get: () => k.keyCode });
    (document.getElementById("canvas") || window).dispatchEvent(ev);
  }

  function setHeld(wanted) {
    for (const code of down) if (!wanted.has(code)) { send("keyup", code); down.delete(code); }
    for (const code of wanted) if (!down.has(code)) { send("keydown", code); down.add(code); }
  }

  function releaseAll() { setHeld(new Set()); }

  function poll() {
    const wanted = new Set();
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const p of pads) {
      if (!p || !p.connected) continue;
      p.buttons.forEach((b, i) => {
        const code = PAD_MAP[i];
        if (code && (b.pressed || b.value > 0.5)) wanted.add(code);
      });
      const [x = 0, y = 0] = p.axes;
      if (x < -AXIS_THRESHOLD) wanted.add("ArrowLeft");
      if (x > AXIS_THRESHOLD) wanted.add("ArrowRight");
      if (y < -AXIS_THRESHOLD) wanted.add("ArrowUp");
      if (y > AXIS_THRESHOLD) wanted.add("ArrowDown");
    }
    setHeld(wanted);
    requestAnimationFrame(poll);
  }

  function toast(text) {
    const el = document.createElement("div");
    el.textContent = text;
    el.style.cssText = "position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:100000;" +
      "background:rgba(0,0,0,.8);color:#fff;font:14px system-ui,sans-serif;padding:8px 14px;border-radius:6px;pointer-events:none";
    document.body.appendChild(el);
    setTimeout(() => el.remove(), 3000);
  }

  window.addEventListener("gamepadconnected", (e) => toast("Controller connected: " + e.gamepad.id));
  window.addEventListener("gamepaddisconnected", () => { releaseAll(); toast("Controller disconnected"); });
  window.addEventListener("blur", releaseAll);
  requestAnimationFrame(poll);
})();
