# Changelog

## v1.0.0 (unreleased)

**Cho Ren Sha 68k at 50 frames a second.** Famibe no Yosshin's 1995 shoot'em up for the Sharp
X68000, the whole game, on an Atari ST, STE, Mega ST or Mega STE with a SidecarTridge
Multi-device.

- **The whole game.** The original's own program runs, translated to C, on the cartridge, and is
  checked against the original at every step:
  - all seven stages and their bosses, and the second loop
  - the music and the effects
  - continue, game over and the name entry
  - the high scores, kept on the card
  - the attract demo, when the title is left alone
- **50 frames a second on every ST**, with up to 512 sprites in a frame.
  - An STE or a Mega STE shows the game's whole 256 x 256 screen, with both borders open.
  - A plain ST or a Mega ST shows its top 240 lines, which hold the game and its score.
- **Sound on every ST.**
  - An STE or a Mega STE plays it through its DMA sound chip, at 25 kHz.
  - A plain ST or a Mega ST plays it through its YM2149, at about 7.7 kHz in eight bits.
- **The controls.**
  - Play with a joystick in port 1, or the cursor keys.
  - Fire or Alternate shoots, and space drops a bomb.
  - ESC pauses the game. While paused, G leaves for the desktop and B for Booster.
  - The title has START, CONTINUE, CONFIG, KAKUSI (the hidden menu, also on K), GEM DESKTOP and
    BOOSTER.
- **The card's files from a web page.** The game's files are made at
  [chorensha.sidecartridge.com](https://chorensha.sidecartridge.com), in your browser, from the
  Amiga port's repository. The page writes them to the card's `CHORENSHA` folder; nothing is
  uploaded.
- **GAME.TOS.** The ST starts to its desktop as usual. Start the game as `GAME.TOS` from the
  cartridge's drive c:, and leave it with GEM DESKTOP to come back to the desktop.
- **A page that says what is missing.** When the card is not ready, the ST says why and shows a
  QR code to the web page for this release. It checks the card every second, so the game starts
  as soon as the files are there. ESC goes back to the desktop, and B starts Booster.

Thanks to Famibe no Yosshin for the game; to Sascha Springer for the Atari Falcon port and the
disassembly it builds on; to Adam Polkosnik for the Amiga port, its repository and its recordings
of the music; and to miniz and Nayuki's QR Code generator.
