# Focused presets game check

Use an identified candidate with its matching shader and preserved live settings.
Choose a save manually. Keep comparisons in the same scene, at the same resolution,
NR placement/pass configuration and frame-generation multiplier. Record the active
weather, game hour and resolved preset names shown in the Neural Rendering tab.

1. **Manual baseline:** use no presets, or Pause presets. Confirm the existing
   image and controls behave normally. Record NR/Image values and a short
   stationary frame-time sample.
2. **Shared assignment:** create a preset, duplicate it, name it, and add
   two selected weather records to it. Change a value and Apply. Verify both
   assignments still point to it; check current-weather assignment and inheritance.
3. **Visible transition:** give clear and rainy weather observably different but
   comfortable tuning. Observe a gradual transition while stationary and moving.
   Check thin foliage, faces and contrast edges for ghosting, flicker, sharpening
   halos or a jump. Confirm the outgoing/incoming names and weather progress.
4. **Clock and loading:** inspect sunrise or sunset and midnight, then waiting,
   fast travel and entering/leaving an interior. Look for stale outdoor tuning,
   a single incorrect frame or prolonged history contamination. Review copied ENB
   anchors against visible lighting; adjust them if needed.
5. **Controls and persistence:** test Pause presets, Apply, Discard,
   Save as default and a manual restart. Check preset names, shared assignments and
   distinct time points survive. Delete a copy and confirm its assignments fall
   back. Check the Image tab reports preset sharpening. Type a preset name and a
   weather search, including Shift and AltGr characters, and verify text editing
   releases input when the menu closes.
6. **Other settings:** give a rain preset one pass and another ! setting such as
   network preset. Check that pass changes are seamless, and note any hitch from
   ! settings at the weather change midpoint.
7. **NR routes and cost:** check the intended linked/independent passes and NR
   placement. Compare paused/active frame-time samples in the same scene,
   alternating order and retaining the actual settings. Community Shaders tests
   NR only. On the ENB route also verify sharpening and its enable control.

Use game weather/time controls only deliberately during a test; a forced immediate
weather is a separate case from a natural transition. Do not claim one validates
the other. The `[Appearance]` log records catalogue size and changes in resolved
preset selection; it is not a frame-by-frame visual or timing measurement.

Record each step as passed, failed or untested. Runtime execution, user visual
feedback and frame-time measurements are separate evidence. Begin with broad
groups; add weather-specific tuning only when the comparison shows a benefit.
