---
name: snapshot
description: Capture screens of the running device as PNGs with tools/snapshot.py and check layout, text fit and translations. Use after any change that affects what a screen shows, for every language, before asking the user for a photo.
---

# Snapshot the screens

1. **Find the IP:** `.devloop/ip`, or the log line `web: Settings page: https://<ip>/`. The PC must be on the same
   network (the cloud shell can't reach the board).
2. **Key:** `tools/snapshot.py` finds it ($FORGE_KEY, `.devloop/key`, or asks the console with `key`). After a flash
   the cached key may be stale: delete `.devloop/key` (L7).
3. **Capture** every screen the change touches (names: `forge.json` `screens`, plus `current` and any pseudo-screens
   the project lists in docs/TESTING.md "Project checks"):
   `python tools/snapshot.py <screen> [out.png] --ip <ip>` (the address defaults to `.devloop/ip`).
4. **Look at each PNG** (Read tool): text clipped by the round panel shows over the red tint; check wrapping,
   overlaps, alignment, empty labels (an empty translation shows nothing, L141).
5. **Other languages:** set the language through the API (save the user's value first):
   `curl -sk -X POST -H "Content-Type: application/json" -H "X-Key: <key>" -d '{"lang":"fr"}' https://<ip>/api/settings`,
   snapshot every screen again, then put the user's language back. French is Québec standard written French;
   long lines need explicit `\n` on a round screen (L142).
6. **Fix and repeat:** shorten in `main/i18n_strings.h`, widen the label or use a smaller font; rebuild and flash
   (`build-flash-probe`), snapshot again.
7. **Limits:** a snapshot shows state, not interaction. For gestures, scrolling, animations and touch targets, ask the
   user to try it during a log window (L49).
