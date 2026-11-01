# Feature ideas

<!-- The backlog. One line per idea, who asked and when; a paragraph only when it needs a decision or a measurement
first. Move an item to "Done" with its version when it ships. -->

## Next

- [ ] **<idea>** — <what the user gets> (<who>, <date>).

## To evaluate

<!-- Ideas that need a measurement or a prototype before deciding ("test it rather than assume"). Record the result
and the decision here. -->

## Done

- [x] **<idea>** — <one line> (vX.Y.Z).
- [x] **Rock paper scissors** — a game page: one tap starts a 3 s countdown on every board, all reveal together (the user, 2026-11-01; v0.5.0).

## Animations over the mesh (the user, 2026-10-31: "once the project is stabilized")

Start with **bicolour waves** coming from the centre, the left or the right. Design, from docs/RESEARCH-BLE.md's
"growth" section: send a description, never frames. The 13 reserved advert bytes carry: effect id (1: 0 = none,
1 = wave), origin (1: centre / left / right), colour B (3; colour A is the board's own colour), period (1, in 50 ms
steps), amplitude or band width (1), start time (2: the sender's 16-bit ms clock) and a shared-clock field so every
board renders the same phase (the ESP-NOW badge swarm pattern: adopt the highest clock heard). Each board draws the
wave as a stateless function of (now - start) through the tear-free raw-frame path (bands filled from a row/column
distance table, two colours blended in linear light), so a 30 fps full-screen wave costs no radio bandwidth. The
hold gesture could pick the wave while a second gesture (swipe up/down on the ring page?) picks the effect. Mixing
rule with several senders (the user, 2026-10-31): the waves **merge like the colours do**: every board renders each
sender's wave (its own included while it holds) and sums them per pixel in linear light, normalised so the largest
channel is full, the same `mp_mix` rule applied per pixel. A 466x466 frame with up to 3 senders is 3 wave
evaluations per pixel: use row/column distance tables and 16-bit fixed point, measure the frame time first.

