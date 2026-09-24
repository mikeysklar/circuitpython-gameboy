# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
"""Play one Game Boy cartridge on a Fruit Jam with CircuitPython + peanutgb.mpy.

John's cartridge model: one ROM per SD card. The first .gb file found on /sd
(or on CIRCUITPY) boots at power on. No menu.

Display: 320x240 RGB565 picodvi framebuffer, which the HSTX doubles to 640x480,
so the game shows at 2x. The emulator writes straight into the framebuffer, no
displayio refresh. Measured 98% of real speed with drawing at the stock 150 MHz,
where a 640x480 buffer does not fit in SRAM and 3x software scaling ran at 39%.

Sound: the TLV320 DAC through adafruit_fruitjam.Peripherals. The emulator writes
each frame's samples into a looping double-buffered RawSample, one half ahead
of playback. Frames are paced to exactly one audio frame (369 samples at
22050 Hz = 59.76 Hz, 0.04% fast), so production matches the DAC's consumption.
If the DAC or library is missing it runs silent.

Controls: a USB gamepad (relic_usb_host_gamepad) and/or the three buttons:
  BUTTON1 left, BUTTON2 right, BUTTON3 A
  BUTTON1+BUTTON2 start, BUTTON1+BUTTON3 down

Serial prints fps every 5 seconds.
"""

import array
import os
import time

import board
import displayio
import keypad
import picodvi
import peanutgb

SOUND = True
AUDIO_OUTPUT = "headphone"  # or "speaker"
VOLUME = 0.5
HALF_FRAMES = 4  # audio ring half = 4 frames (~67 ms); latency ~1 half


def find_rom():
    for d in ("/sd", "/"):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            if n.lower().endswith(".gb") and not n.startswith("."):
                return d.rstrip("/") + "/" + n
    raise OSError("no .gb cartridge on /sd or CIRCUITPY")


# ---- display -------------------------------------------------------------------
displayio.release_displays()
PINS = dict(
    clk_dp=board.CKP, clk_dn=board.CKN, red_dp=board.D0P, red_dn=board.D0N,
    green_dp=board.D1P, green_dn=board.D1N, blue_dp=board.D2P, blue_dn=board.D2N,
)
FB_W, FB_H = 320, 240
try:
    fb = picodvi.Framebuffer(FB_W, FB_H, color_depth=16, **PINS)
    BPP = 16
except (MemoryError, ValueError):
    fb = picodvi.Framebuffer(FB_W, FB_H, color_depth=8, **PINS)
    BPP = 8
buf = memoryview(fb)  # item size follows the color depth
for i in range(len(buf)):
    buf[i] = 0
del buf

# ---- cartridge -----------------------------------------------------------------
path = find_rom()
with open(path, "rb") as f:
    rom = f.read()
print("cartridge", path, len(rom), "bytes")

st = bytearray(peanutgb.STATE_SIZE)
err = peanutgb.init(st, rom)
if err:
    raise RuntimeError("peanutgb init error %d" % err)

save_path = path.rsplit(".", 1)[0] + ".sav"
save_size = peanutgb.save_size(st)
cart = None
if save_size:
    cart = bytearray(save_size)
    try:
        with open(save_path, "rb") as f:
            f.readinto(cart)
        print("loaded", save_path)
    except OSError:
        pass
    peanutgb.set_cart_ram(st, cart)

peanutgb.set_output(st, fb, FB_W, BPP, 1, (FB_W - 160) // 2, (FB_H - 144) // 2)

# ---- sound ---------------------------------------------------------------------
RATE = peanutgb.AUDIO_RATE
PER_FRAME = peanutgb.AUDIO_SAMPLES_PER_FRAME
FRAME_NS = PER_FRAME * 1_000_000_000 // RATE
ring = None
audio = None
try:
    if not SOUND:
        raise RuntimeError("SOUND = False")
    import audiocore
    import digitalio
    import adafruit_fruitjam

    reset = digitalio.DigitalInOut(board.PERIPH_RESET)
    reset.switch_to_output(False)
    time.sleep(0.1)
    reset.value = True
    periphs = adafruit_fruitjam.Peripherals(
        audio_output=AUDIO_OUTPUT, sample_rate=RATE, bit_depth=16, i2c=board.I2C()
    )
    periphs.volume = VOLUME
    # Peripherals claims BUTTON1-3 as DigitalInOut; hand the pins to keypad.
    for b in getattr(periphs, "_buttons", ()):
        b.deinit()
    audio = periphs.audio
    ring = array.array("h", bytes(2 * HALF_FRAMES * PER_FRAME * 4))
    sample = audiocore.RawSample(ring, channel_count=2, sample_rate=RATE, single_buffer=False)
except Exception as e:  # no DAC, no library, wrong board: play silent
    print("sound off:", e)
    ring = None

RING_FRAMES = 2 * HALF_FRAMES * PER_FRAME  # stereo frames in the ring

# ---- controls ------------------------------------------------------------------
keys = keypad.Keys((board.BUTTON1, board.BUTTON2, board.BUTTON3), value_when_pressed=False, pull=True)
held = [False, False, False]

gamepad = None
pad_mask = 0
PAD_MAP = {}
try:
    import relic_usb_host_gamepad as rg

    gamepad = rg.Gamepad()
    PAD_MAP = {
        rg.BUTTON_UP: peanutgb.JOYPAD_UP,
        rg.BUTTON_JOYSTICK_UP: peanutgb.JOYPAD_UP,
        rg.BUTTON_DOWN: peanutgb.JOYPAD_DOWN,
        rg.BUTTON_JOYSTICK_DOWN: peanutgb.JOYPAD_DOWN,
        rg.BUTTON_LEFT: peanutgb.JOYPAD_LEFT,
        rg.BUTTON_JOYSTICK_LEFT: peanutgb.JOYPAD_LEFT,
        rg.BUTTON_RIGHT: peanutgb.JOYPAD_RIGHT,
        rg.BUTTON_JOYSTICK_RIGHT: peanutgb.JOYPAD_RIGHT,
        rg.BUTTON_A: peanutgb.JOYPAD_A,
        rg.BUTTON_B: peanutgb.JOYPAD_B,
        rg.BUTTON_X: peanutgb.JOYPAD_B,
        rg.BUTTON_Y: peanutgb.JOYPAD_A,
        rg.BUTTON_START: peanutgb.JOYPAD_START,
        rg.BUTTON_SELECT: peanutgb.JOYPAD_SELECT,
    }
except Exception as e:
    print("gamepad off:", e)
    gamepad = None


def joypad():
    global pad_mask
    ev = keys.events.get()
    while ev:
        held[ev.key_number] = ev.pressed
        ev = keys.events.get()
    if gamepad is not None and gamepad.update():
        for e in gamepad.events:
            bit = PAD_MAP.get(e.key_number, 0)
            if e.pressed:
                pad_mask |= bit
            else:
                pad_mask &= ~bit
    b1, b2, b3 = held
    if b1 and b2:
        m = peanutgb.JOYPAD_START
    elif b1 and b3:
        m = peanutgb.JOYPAD_DOWN
    else:
        m = 0
        if b1:
            m |= peanutgb.JOYPAD_LEFT
        if b2:
            m |= peanutgb.JOYPAD_RIGHT
        if b3:
            m |= peanutgb.JOYPAD_A
    return m | pad_mask


# ---- run -----------------------------------------------------------------------
t_play = time.monotonic_ns()
if ring is not None:
    audio.play(sample, loop=True)
    t_play = time.monotonic_ns()
    peanutgb.set_audio(st, ring, HALF_FRAMES * PER_FRAME)  # one half ahead

frames = 0
t_emu = 0  # ns inside run_frames
t_input = 0  # ns reading buttons and gamepad
t_report = t_play
next_frame = t_play
try:
    while True:
        t0 = time.monotonic_ns()
        peanutgb.set_joypad(st, joypad())
        t1 = time.monotonic_ns()
        if peanutgb.run_frames(st, 1) != 1:
            print("core error", peanutgb.error(st))
            break
        now = time.monotonic_ns()
        t_input += t1 - t0
        t_emu += now - t1
        frames += 1
        if now - t_report >= 5_000_000_000:
            print("fps %.1f  emu %.2f ms  input %.2f ms  per frame" % (
                frames * 1e9 / (now - t_report), t_emu / frames / 1e6, t_input / frames / 1e6))
            frames = 0
            t_emu = t_input = 0
            t_report = now
        next_frame += FRAME_NS
        if next_frame > now:
            time.sleep((next_frame - now) / 1e9)
        else:
            # Running slow. Re-anchor pacing and, if we fell more than a half
            # behind the DAC, move the write point back one half ahead of the
            # estimated play position so stale audio is not replayed as noise.
            behind = (now - next_frame) // FRAME_NS
            next_frame = now
            if ring is not None and behind >= HALF_FRAMES:
                played = (now - t_play) * RATE // 1_000_000_000
                start = (played + HALF_FRAMES * PER_FRAME) % RING_FRAMES
                peanutgb.set_audio(st, ring, start)
finally:
    if audio is not None:
        audio.stop()
    if cart:
        try:
            with open(save_path, "wb") as f:
                f.write(cart)
            print("saved", save_path)
        except OSError as e:
            print("save failed", e)
