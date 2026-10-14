# Cat Tracker: notes for Claude Code

Start with `HANDOFF.md` (current state, what is untested, open questions), then `PROTOCOL.md`.

- `firmware/common/` is the source of truth for `protocol.h` and `config_json.h`. After editing, run
  `firmware/sync_common.sh`. Keep every LoRa packet at 24 bytes or less (US 400 ms dwell at SF10/125 kHz).
- Sketch code lives in `main.cpp`; leave the `.ino` files empty.
- Any change to the JSON messages must be made in the firmware, `app/src/store.ts`, `app/src/demo.ts` and
  `PROTOCOL.md` together.
- After firmware changes, rebuild `firmware/bin/` (see HANDOFF.md, Building) so the ready-to-flash files match the source.
- App: `cd app && npm run build` must pass (it type-checks). Pushing `app/**` to `main` redeploys GitHub Pages.
- Do not decide the items under "Open questions for Devon" in HANDOFF.md without asking him.
- Public docs and UI text: no em dashes.
