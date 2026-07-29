# V4L2 Trigger Mode Recovery Design

**Date:** 2026-07-29

## Goal

Prove whether the four Nori UVC cameras can be changed from
`HARDWARE_TRIGGER_MODE` (`2`) to `NON_TRIIGER_MODE` (`0`) without using
`Nori_Xvision_SetTriggerMode`, then verify that each camera produces an MJPEG
stream through V4L2.

This is an isolated recovery experiment. It does not yet replace the production
capture pipeline.

## Evidence

- The four cameras enumerate correctly through `uvcvideo`.
- `Nori_Xvision_GetTriggerMode` reports mode `2` for every camera.
- `Nori_Xvision_SetTriggerMode(device, 0)` returns `NORI_OK`, but an immediate
  readback remains `2`.
- The vendor-provided `grab_image` arm64 sample reproduces the same behavior.
- Direct V4L2 streaming succeeds at about 30 fps for JHHSW, while SL completes
  `VIDIOC_STREAMON` but returns no frames in mode `2`.
- Standard V4L2 controls do not expose trigger mode. Each device does expose a
  vendor UVC extension unit named `Extension 4`.
- The same physical devices passed at the vendor approximately two hours before
  this investigation.

## Recommended Experiment

1. Record the complete USB descriptors for each device, including extension
   unit GUIDs and control bitmaps.
2. Trace the vendor SDK's `GetTriggerMode` and `SetTriggerMode` operations at the
   ioctl/USB-control boundary.
3. Identify the extension-unit ID, selector, payload length, and GET/SET
   request format used for trigger mode.
4. Implement a small standalone diagnostic program around
   `UVCIOC_CTRL_QUERY`.
5. Test one device first:
   - read current mode;
   - write mode `0`;
   - read the mode back;
   - stream 60 MJPEG frames with V4L2.
6. Only after the single-device test passes, repeat for all four devices.

## Safety Boundaries

- Do not flash firmware.
- Do not overwrite the installed Nori SDK.
- Do not modify the production capture path during the experiment.
- Do not send guessed extension-unit payloads. A SET request is allowed only
  after its unit, selector, length, and payload semantics have been observed
  from the vendor SDK or authoritative descriptors.
- Preserve the current clean Git baseline and keep the experiment independently
  buildable and removable.

## Success Criteria

- All four devices read back trigger mode `0`.
- Each corresponding V4L2 video node produces at least 60 consecutive MJPEG
  frames at an observed rate close to 30 fps.
- Device matching uses USB product and serial information, not unstable
  `/dev/videoN` numbering.

## Failure Handling

If the SDK does not issue a visible UVC extension-unit transaction, or the
observed transaction still cannot change the mode, stop before guessing vendor
commands. Preserve the trace and provide it to Nori together with the firmware
versions and the official-sample reproduction.

