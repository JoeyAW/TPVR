# JAudio

**JAudio** is the name for the audio engine used by Twilight Princess (along with many other Nintendo games from the era). TP uses exclusively JAudio v2, while other games use v1 or a mix of v1 and v2 infrastructure. This document will primarily focus on TP's use case, but best efforts will be made to document where behavior is TP-specific.

## Common concepts

Audio is almost exclusively 

### `JAISoundID`

A "sound", be that a _sound effect_ or music, is referenced in-code through the `JAISoundID` type. This is a `u32` that the game uses to look up the relevant sound. The actual value is bitpacked (BE) from the following fields:

| Type  | Field      | Description                                                                                                                                                                                               |
|-------|------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `u8`  | Section ID | Section of the Sound Table this sound effect is located in. 0 for sound effects, 1 for sequenced music, 2 for streamed music.[^sectionids]                                                                |
| `u8`  | Group ID   | ID of group further used to organize inside the Sound Table's Section. Sound effects are grouped into things like "SYSTEM" and "ENEMY", other sections leave this at 0.                                   |
| `u16` | "Wave ID"  | Index inside the group to look up the sound at.<br/>**Note** that the term "wave" in code is extremely confusing[^waveterm]; it does **not** refer to audio samples ("waves") in the wave banks directly. |

[^sectionids]: JAudio itself can seemingly work outside this convention, however it is enforced by some TP-specific game code.

[^waveterm]: Possibly vestigial from JAudio v1, where I believe there were less layers of indirection.

## Disc files

All audio data is stored in `/Audiores` on the disc. Files are as follows:

### `/Audiores/Seqs/Z2SoundSeqs.arc`

Contains the BMS instructions for all BMS-based music and sound effects. Not all data is kept in memory at once.

### `/Audiores/Stream/*.ast`

Contains individual streamed music. Each file is a separate music track.  See [this page](https://www.lumasworkshop.com/wiki/AST_(File_Format)) for file format description.

### `/Audiores/Waves/*.aw`

Contains audio sample data for sequenced music and sound effects. These files are pure meat, no bone: they are loaded directly into ARAM and all metadata is stored in the BAA WSYS sections.

Each file contains audio samples for one "scene", with factors like the current level determining what "scenes" are made resident in memory. There is tons of duplicate data between scenes, presumably to increase simplicity and loading performance.

### `/Audiores/Z2Sound.baa`

Contains all remaining metadata for the audio system. This is effectively a container for a bunch of different sub-sections. The file starts with a bunch of "commands" that indicate where other data in the file is. Each command has a 4-character identifier and depending on the command will be followed by some extra arguments before the next command.

The commands used by TP's BAA file are as follows (note that the decompiled code has support for more load commands, which are unused):

| Command/Argument | Value           | Description                                                                                                                               |
|------------------|-----------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| Command          | `AA_<`          | Start of BAA commands. Must be at the start of the file and only appear once.                                                             |
| Command          | `>_AA`          | End of BAA commands.                                                                                                                      |
| Command          | `ws  `[^spaces] | Wave bank/WSYS data. Defines where audio samples are located on disc.                                                                     |
| Argument         | u32             | Wave bank ID, max 255. In TP, 0 is sound effects, 1 is music samples.                                                                     |
| Argument         | u32             | File offset for start of `WSYS` data                                                                                                      |
| Argument         | u32             | Bit field selecting which groups (= `.aw` files) to load immediately. TP leaves this at zero.[^32ws]                                      |
| Command          | `bnk `[^spaces] | Instrument bank/IBNK                                                                                                                      |
| Argument         | u32             | Target wave bank ID                                                                                                                       |
| Argument         | u32             | File offset for start of `IBNK` data                                                                                                      |
| Command          | `bsc `[^spaces] | Sound effect sequence collection.                                                                                                         |
| Argument         | u32             | File offset for start of `SC` data.                                                                                                       |
| Argument         | u32             | File offset for end of `SC` data.                                                                                                         |
| Command          | `bst `[^spaces] | Sound table. Defines parameters for music and sound effects.                                                                              |
| Argument         | u32             | File offset for start of `BST `[^spaces] data.                                                                                            |
| Argument         | u32             | File offset for end of `BST `[^spaces] data.                                                                                              |
| Command          | `bstn`          | Sound name table. Defines names of all music and sound effects. Present on disc, but loading is disabled on release versions of the game. |
| Argument         | u32             | File offset for start of `BSTN` data.                                                                                                     |
| Argument         | u32             | File offset for end of `BSTN` data.                                                                                                       |
| Command          | `bfca`          | Unknown, something related to initialization of DSP FX data.                                                                              |
| Argument         | u32             | File offset for start of `RARC` data.                                                                                                     |

[^spaces]: Padded with spaces.

[^32ws]: Both of TP's wave banks have far more than 32 groups, and seemingly this mechanism would not be able to deal with that.

Following is data descriptions for the remaining data in the `.BAA`, as pointed to by the above commands.

### `WSYS` / Wave banks

`WSYS` / Wave bank data defines where a set of audio samples can be found on disc. Each wave bank is made of multiple "groups", where each group corresponds to one `.aw` file on disc. Each group has a set of "wave IDs" it contains, along with the metadata (e.g. sample rate) and data offset in the `.aw` file.

Multiple groups can contain the same wave ID, thus meaning the raw audio samples can be duplicated on disk.

For the actual binary layout of this data, check [the ImHex pattern](imhex/jaudio/wsys.hexpat)

*Relevant classes: `JASWSParser`, `JASBasicWaveBank`, `JASSimpleWaveBank`.*

## Further reading & credits

* https://www.lumasworkshop.com/wiki/SMR.szs (note that details like the exact layout of the BAA are not the same as TP)
* XAYRGA for doing much RE work and making [JAMTools](https://xayr.gay/tools/SoundModdingToolkit/)
* The decompiled source code, duh.