# Plan to implement Satellite features

Radios with the OpenGD77 firmware as well as some Anytone radios support satellite features. No need to boot into Windows just to update the radio's Keplerian data.

OpenGD77 CPS Keplerian data URL reported from the installed CPS:
https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle

- [x] Research how OpenGD77 updates radio's data
  - [x] Use the same URL OpenGD77 uses (but make it configureable)
  - [x] See which satellites OpenGD77 has by default. See which ones are still online. Remove those that are not. Add additional.
  - [x] See how OpenGD77 saves the Keplerian data to the radio
- [x] Add a button within Radio Settings for satellites. This button will only appear for supported radios
- [x] Declare capabilities in the WCP. Satellite support, capacity, encoding, and write instructions should come from the definition rather than model-specific Waffle code.
- [x] The button will display a new window/dialog
- [x] The first time this is clicked, a file will be saved with some default satellites
- [x] User should be able to update this list in the GUI which will update the file
- [x] Have a button to update the radio's data
- [x] The same button will also set the radio's time
- [x] Updating satellite data will require a successful internet download. If it fails, Waffle will show an error and leave the radio unchanged.
- [x] When updating the radio, combine the user’s satellite list with freshly downloaded Keplerian data, matching entries by satellite catalog ID.
- [x] After Keplerian data is downloaded and the new data is successfully written to the radio, change the satellites that had data in the download to green. Leave any unsuccessful satellites as uncolored.
- [x] Keep satellite writes separate from codeplug writes. Preserve other settings sharing the same memory sectors and verify the written data where supported.
- [ ] Research if we can support other radios using this same method (even if they need a different format or different radio write)
  - [ ] AnyTone AT-D168UV
  - [ ] AnyTone AT-D890UV
    - [ ] BTECH DA-7X2 (rebranded variant)
  - [ ] AnyTone AT-D878UVII Plus
    - Satellite features were available in firmware 3.x but removed in firmware 4.00. Support must account for the installed firmware.
  - [ ] BTECH DMR-6X2
  - [ ] BTECH DMR-6X2 PRO
