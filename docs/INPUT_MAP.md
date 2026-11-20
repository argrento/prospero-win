# Input map

`map = 1` in a profile's `[input]` section turns on the input map. The map
says what each DualSense input sends to the game: a controller input, keys,
the mouse, or nothing. An input that the map sends somewhere else leaves the
controller: the game gets only what the map sends.

A profile without `map = 1` works as described in [CONTROLS.md](CONTROLS.md),
with no change. Kits that read this grammar list the `input-map` capability.

This page is the grammar. It is strict, as the rest of the profile is: an
unknown key, a bad value, a repeated key or a line in the wrong place refuses
the whole profile.

## Example

```ini
[input]
mode = keyboard
map = 1
cross = key:space
circle = key:ctrl
square = key:r
triangle = key:e
l2 = mouse:right
r2 = mouse:left
r2_threshold = 30
up = mouse:wheel_up
down = mouse:wheel_down
left_stick = keys
left_stick_keys = w,s,a,d
right_stick = mouse
right_stick_speed = 1500
```

## Where the lines go

- `map = 1` goes in the profile's own `[input]` section, or in `[launcher]`
  (below). The only value is `1`. To turn the map off, leave the line out.
- `map = 1` comes before every input line and tuning line (below). An input
  line before it is read as an old-style line, and the profile is refused.
- These keys keep their meaning with `map = 1`: `mode`, `rumble`, and in
  `[input]` only, `player2`.
  In a section with `map = 1` they are written as strictly as the map's own
  lines (below), before `map = 1` as well as after it: `MODE = KEYBOARD` or
  `rumble = 050` refuses the profile.
- These keys are refused with `map = 1`: `preset`, `mouse`, `mouse_speed`.
  The stick lines replace `mouse` and `mouse_speed`.
- An old-style button value (`cross = z`, `r2 = mouse_left`, `vk:0x41`
  without `key:`) is refused with `map = 1`. Every target has the form given
  under [Targets](#targets); `none` is written the same in both grammars.
- `map = 1` is refused in an input preset file, which keeps the old grammar.
  `[launcher]` takes either grammar: its own `map = 1` and lines, with the
  same rules as `[input]` (without `player2`), or the old lines (kits that read a map there list
  the `launcher-map` capability). When the touchpad hold or a launcher swaps
  to the `[launcher]` input, its map (or its old lines) plays until it swaps
  back, and `[input]`'s map plays again after.
- Each key comes once. Keys and values are lower case. A number has no
  leading zero (`16`, not `016`), and a value has no space inside it
  (`w,s,a,d`, not `w, s, a, d`; `key:ctrl+s`, not `key:ctrl + s`).
- No value holds `;`, `=`, `#`, a space inside a name, or a line break. Keys
  are always written by name (`semicolon`, `equals`, `comma`), never as the
  character.

## Inputs

An input line is `<input> = <target>`. These are the inputs, in the order
that a writer uses:

| Input | DualSense | Kind |
| --- | --- | --- |
| `cross`, `circle`, `square`, `triangle` | face buttons | button |
| `l1`, `r1` | shoulder buttons | button |
| `l2`, `r2` | triggers | trigger |
| `l3`, `r3` | stick clicks | button |
| `up`, `down`, `left`, `right` | d-pad | button |
| `options` | Options | button |
| `create` | Create | button |
| `touchpad_left` | a one-finger touchpad click that began on the left half | button |
| `touchpad_right` | a one-finger touchpad click that began on the right half | button |
| `left_stick`, `right_stick` | sticks | stick |

`touchpad` is not an input with `map = 1`: map the two halves.

### Reserved

These never reach the map, and no line can name them:

- **PS.** It stays the console's.
- **The two-finger touchpad click.** It opens the console's keyboard.
- **Options + Create held for three seconds.** It closes the game. Options
  and Create can each be mapped; their mapped actions are sent while they are
  held, and the close still reads the real pad.
- **L3 + R3 pressed together.** It opens the in-game menu when `[display]`
  has `menu = true`. L3 and R3 can each be mapped.

Create can be mapped, but pressing it also opens the PS5's capture menu.

Lines name no player. The map is the first player's. Other players'
controllers and a `player2` preset work as without the map.

## Targets

### Button targets

A button input takes one of these:

| Target | What the game gets |
| --- | --- |
| `pad:<button>` | a controller button, from the table below |
| `key:<key>` | a key |
| `key:<key>+<key>` | a combination of up to 4 keys |
| `mouse:left`, `mouse:right`, `mouse:middle` | a mouse button |
| `mouse:back`, `mouse:forward` | the side mouse buttons (Windows X1 and X2) |
| `mouse:wheel_up`, `mouse:wheel_down` | one wheel notch (120 units) for each press |
| `none` | nothing |

A key combination is pressed in the order written and released in the
opposite order. The wheel does not repeat while the button is held.

When more than one input holds the same key (two combinations with `ctrl`,
or a button and a stick direction on `w`), the key goes down with the first
and comes up when the last of them is let go. A mouse button goes down and
up with each input that sends it, as the old-style lines send it.

A press of an input that the game still has down sends its release first:
a button let go and pressed again between two reads of the controller is
two presses.

### Controller buttons

`mode` decides which controller the game has. The names are the same in
every mode, and the layout is checked the same way in every mode. In
`keyboard` mode the game has no controller that receives input, so `pad:`
targets send nothing.

| Name | `mode = xinput` | `mode = playstation` |
| --- | --- | --- |
| `pad:a` | A | Cross |
| `pad:b` | B | Circle |
| `pad:x` | X | Square |
| `pad:y` | Y | Triangle |
| `pad:lb` | LB | L1 |
| `pad:rb` | RB | R1 |
| `pad:lt` | LT | L2 |
| `pad:rt` | RT | R2 |
| `pad:ls` | left stick click | L3 |
| `pad:rs` | right stick click | R3 |
| `pad:dpad_up`, `pad:dpad_down`, `pad:dpad_left`, `pad:dpad_right` | d-pad | d-pad |
| `pad:start` | Start | Options |
| `pad:back` | Back | Share |
| `pad:guide` | Guide (the Xbox button) | nothing |
| `pad:touchpad` | nothing | touchpad click |

`pad:lt` and `pad:rt` from a button give the trigger fully pulled while the
button is held. From a trigger they give the trigger's analog pull.

When more than one input sends the same controller button, the button is down
while any of them is held. A trigger gets the largest pull sent to it.

In `playstation` mode a finger on the touchpad still reaches the DualShock 4's
touchpad, as without the map.

When the profile's `[launcher]` preset is in `playstation` mode, the game's
controller is a DualShock 4 under every preset. A map in `xinput` mode then
gives it the buttons it has: `pad:guide` sends nothing.

### Keys

A key name is one of these:

- a letter `a` to `z`, or a digit `0` to `9` (the main row);
- `f1` to `f24`;
- `space`, `enter`, `escape`, `tab`, `backspace`, `capslock`, `pause`,
  `printscreen`, `scrolllock`, `numlock`, `insert`, `delete`, `home`, `end`,
  `pageup`, `pagedown`, `up`, `down`, `left`, `right`, `apps` (the menu key);
- `semicolon`, `equals`, `comma`, `minus`, `period`, `slash`, `backquote`,
  `lbracket`, `backslash`, `rbracket`, `quote`;
- the number pad: `num0` to `num9`, `multiply`, `add`, `subtract`,
  `decimal`, `divide`;
- modifiers: `ctrl`, `lctrl`, `rctrl`, `shift`, `lshift`, `rshift`, `alt`,
  `lalt`, `ralt`, `lwin`, `rwin`;
- `vk:0xNN`: a Windows virtual key code as two lower-case hex digits, not
  `00`, for a key that has no name in this list. A code that has a name is
  refused (`vk:0x20` is `space`).

| Name | Code | Name | Code | Name | Code |
| --- | --- | --- | --- | --- | --- |
| `ctrl` | 0x11 | `capslock` | 0x14 | `num0`-`num9` | 0x60-0x69 |
| `lctrl` | 0xa2 | `printscreen` | 0x2c | `multiply` | 0x6a |
| `rctrl` | 0xa3 | `scrolllock` | 0x91 | `add` | 0x6b |
| `shift` | 0x10 | `numlock` | 0x90 | `subtract` | 0x6d |
| `lshift` | 0xa0 | `apps` | 0x5d | `decimal` | 0x6e |
| `rshift` | 0xa1 | `lwin` | 0x5b | `divide` | 0x6f |
| `alt` | 0x12 | `rwin` | 0x5c | | |
| `lalt` | 0xa4 | `ralt` | 0xa5 | | |

The other names have the codes listed in `src/pw_game_profile.c`.

A combination (`key:ctrl+shift+s`) has 1 to 4 names joined by `+`, each name
once. Modifiers come first, in the order of the modifier list above; then the
other keys. `key:shift+ctrl+s` and `key:s+ctrl` are refused.

### Triggers

`l2` and `r2` take `pad:lt` or `pad:rt` (the analog pull), or any other
button target. With a button target the trigger works as a button:

| Line | Values | Default |
| --- | --- | --- |
| `<trigger>_threshold` | 5 to 100: percent of a full pull | the trigger's own click point |

With a threshold, the button goes down when the pull reaches the threshold
(raw `threshold * 255 / 100`, rounded down) and comes up when it falls to
5 points below it (raw `(threshold - 5) * 255 / 100`, rounded down, or less).
Every pull the controller reports counts, also when several come between two
reads: a pull past the threshold and back within one read is a press and a
release. Without the line, the button
follows the DualSense's own L2 or R2 button, as an old-style `r2 = ...` line
does. `<trigger>_threshold` is refused with `pad:lt`, `pad:rt` and `none`,
and when the trigger has no line (its default is `pad:lt` or `pad:rt`).

### Sticks

A stick input takes a mode:

| Mode | What the game gets |
| --- | --- |
| `pad:left_stick`, `pad:right_stick` | a controller stick |
| `pad:dpad` | the controller's d-pad, one direction per axis past the dead zone |
| `keys` | four keys, one direction per axis past the dead zone |
| `mouse` | the pointer moves |
| `wheel` | the vertical axis turns the mouse wheel |
| `none` | nothing |

Each mode takes some of these tuning lines. A tuning line that the stick's
mode does not use is refused; `none` uses none of them. A stick with no line
has its default mode (`pad:left_stick` or `pad:right_stick`), and its tuning
lines are checked against that mode. The modes are checked once the whole
section is read, so a tuning line may come before its stick's line.

| Line | Values | `pad:*_stick` | `pad:dpad`, `keys` | `mouse` | `wheel` |
| --- | --- | --- | --- | --- | --- |
| `<stick>_deadzone` | 0 to 90, percent | 0 | 40 (10 to 90) | 16 | 16 |
| `<stick>_outer` | 0 to 50, percent | 0 | refused | 0 | 0 |
| `<stick>_curve` | `linear`, `squared`, `cubed` | `linear` | refused | `squared` | `squared` |
| `<stick>_speed` | see below | refused | refused | 1200 (1 to 20000) | 10 (1 to 100) |
| `<stick>_invert_x` | `true`, `false` | `false` | `false` | `false` | refused |
| `<stick>_invert_y` | `true`, `false` | `false` | `false` | `false` | `false` |
| `<stick>_keys` | four key names | refused | `keys` only, required | refused | refused |

- **Axis values.** An axis reads 0 to 255 with its centre at 128. Its
  deflection is `value - 128` (-128 to 127) and its size is the deflection
  without its sign.
- **Dead zone and outer dead zone.** The dead zone in raw units is
  `deadzone * 127 / 100`, rounded down (16 gives 20). The outer zone is
  `outer * 127 / 100`. A size at or below the dead zone is 0; a size at
  `127 - outer` or more is full; between them the size is scaled linearly to
  0 to 1. `deadzone + outer` is at most 90.
- **Shape.** For `pad:*_stick` with dead zone 0, outer zone 0 and `linear`,
  each axis reaches the controller untouched, so a corner stays a corner. With
  any of them set, the size is the stick's distance from the centre
  (`sqrt(dx*dx + dy*dy)`), it is scaled and curved as below, and both axes are
  multiplied by `result * 127 / size`, then each axis is clamped to -128 to
  127. The direction is kept. For
  `mouse` and `wheel` they apply to each axis on its own, as the pointer stick
  does without the map. For `pad:dpad` and `keys` each axis is checked on its
  own, so a diagonal holds two directions.
- **Curve.** The scaled size is raised to the power 1 (`linear`), 2
  (`squared`) or 3 (`cubed`).
- **Hysteresis.** In `pad:dpad` and `keys`, a direction comes on when its
  axis size is more than the raw dead zone, and goes off when it falls to
  `(deadzone - 5) * 127 / 100` (rounded down) or less.
- **Speed.** In `mouse`: pixels per second at full tilt; fractions of a pixel
  carry over to the next tick. In `wheel`: notches (120 units) per second at
  full tilt; whole notches are sent, and the rest carries over. Pushing up
  turns the wheel up.
- **Invert.** `invert_x` swaps left and right; `invert_y` swaps up and down.
- **Keys.** `<stick>_keys = <up>,<down>,<left>,<right>`: four entries
  joined by commas. Each is one key name from [Keys](#keys) (modifiers and
  `vk:0xNN` included, without `key:` and without `+`), or `none` for a
  direction that sends nothing. A name can't come twice; `none` can.
  `w,s,a,d` is the usual one.

With the defaults, `pad:left_stick` on the left stick gives the game the
stick exactly as without the map, and `mouse` moves the pointer exactly as
`mouse = left_stick` does.

When both sticks send to the same controller stick, the two are added and
each axis is clamped.

## A missing line

With `map = 1`, an input with no line sends what it sends without the map:

| Input | Default |
| --- | --- |
| a button | its own controller button: `cross` `pad:a`, `circle` `pad:b`, `square` `pad:x`, `triangle` `pad:y`, `l1` `pad:lb`, `r1` `pad:rb`, `l3` `pad:ls`, `r3` `pad:rs`, the d-pad `pad:dpad_*`, `options` `pad:start`, `create` `pad:back` |
| `touchpad_left` | `pad:back` in `xinput` mode, `pad:touchpad` in `playstation` mode |
| `touchpad_right` | `pad:guide` in `xinput` mode, `pad:touchpad` in `playstation` mode |
| `l2`, `r2` | `pad:lt`, `pad:rt` |
| `left_stick`, `right_stick` | `pad:left_stick`, `pad:right_stick` |

In `keyboard` mode these send nothing, since `pad:` targets send nothing
there. A tuning line that is missing has its default.

A line that states the default is accepted and means the same as no line.
A writer writes, after `map = 1`:

- one line for every input, in the order of the input table;
- right after an input's line, its tuning lines that differ from the
  default, in this order: `_threshold`, `_keys`, `_deadzone`, `_outer`,
  `_curve`, `_speed`, `_invert_x`, `_invert_y`.

## Releasing what is held

Everything the map holds is let go when:

- the touchpad hold or a launcher swaps presets;
- the controller is lost (it disconnects, or the system overlay takes it);
- the in-game menu or the console's keyboard opens.

Let go means: keys and mouse buttons come up, the controller reads idle, and
the pointer and wheel remainders are cleared. An input still held when the
menu or the keyboard closes stays away from the game until it is let go:

- a button, until it comes up (or is pressed again);
- a stick in `pad:dpad` or `keys`, each direction until it goes off;
- a stick in `pad:*_stick`, `mouse` or `wheel`, until both its axes are back
  within 16 % of the centre (`16 * 127 / 100`, 20 raw), or within its dead
  zone when that is larger;
- a trigger sending `pad:lt` or `pad:rt`, until its pull is 5 % or less
  (`5 * 255 / 100`, 12 raw).

## Templates

These give the same input as the old presets with the same name. Each one
names the `mode` it is for; `mode` is not part of the map.

### Gamepad (`mode = xinput`)

```ini
map = 1
cross = pad:a
circle = pad:b
square = pad:x
triangle = pad:y
l1 = pad:lb
r1 = pad:rb
l2 = pad:lt
r2 = pad:rt
l3 = pad:ls
r3 = pad:rs
up = pad:dpad_up
down = pad:dpad_down
left = pad:dpad_left
right = pad:dpad_right
options = pad:start
create = pad:back
touchpad_left = pad:back
touchpad_right = pad:guide
left_stick = pad:left_stick
right_stick = pad:right_stick
```

### Mouse (`mode = keyboard`)

```ini
map = 1
cross = mouse:left
circle = mouse:right
square = mouse:middle
triangle = none
l1 = none
r1 = none
l2 = mouse:right
r2 = mouse:left
l3 = none
r3 = none
up = none
down = none
left = none
right = none
options = key:enter
create = none
touchpad_left = none
touchpad_right = none
left_stick = mouse
left_stick_speed = 900
right_stick = none
```

### Arrow keys (`mode = keyboard`)

```ini
map = 1
cross = key:z
circle = key:x
square = key:shift
triangle = key:c
l1 = none
r1 = none
l2 = none
r2 = none
l3 = none
r3 = none
up = key:up
down = key:down
left = key:left
right = key:right
options = key:enter
create = none
touchpad_left = none
touchpad_right = none
left_stick = none
right_stick = none
```

### WASD and mouse (`mode = keyboard`)

For games played with the keyboard and the mouse: the left stick walks, the
right stick aims, R2 fires and L2 aims down the sights.

```ini
map = 1
cross = key:space
circle = key:ctrl
square = key:r
triangle = key:e
l1 = key:q
r1 = key:f
l2 = mouse:right
r2 = mouse:left
l3 = key:shift
r3 = key:v
up = mouse:wheel_up
down = mouse:wheel_down
left = key:1
right = key:2
options = key:escape
create = key:tab
touchpad_left = key:m
touchpad_right = key:i
left_stick = keys
left_stick_keys = w,s,a,d
right_stick = mouse
```
