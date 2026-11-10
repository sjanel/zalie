# Interface code and its tests

- `src/ui` has **no platform or GL calls**: textures go through Dear ImGui's texture protocol, and the host
  (`src/app/main.cpp`) feeds the events.
- Dear ImGui is 1.93: `GetContentRegionMax` is gone, `PathStroke` / `AddRect` take flags last, some helpers are internal
  (`imgui_internal.h`).
- **No CPU when idle**: the loop only renders continuously while `App::animating()` is true. A thread with something new
  to show wakes the loop through a `wakeFn` (`Executor::Options`, `glfwPostEmptyEvent` in the app) instead of making it
  poll.

## Headless tests

The tests run the real interface on the null backends of Dear ImGui: no window, no GPU, no display. Details in
`docs/development/testing.md`.

- `tests/ui/ui-test-fixture.hpp` injects the inputs between frames and turns every Dear ImGui assertion into a test
  failure. It records the items of each frame through Dear ImGui's test engine hooks (`tests/ui/imgui-test-hooks.*`,
  compiled into `zalie_imgui` with `IMGUI_ENABLE_TEST_ENGINE` only when `ZALIE_BUILD_TESTS` is ON): find an item by
  the label given to Dear ImGui, `##id` part included (`findItem`, `hoverItem`, `clickItem`, `dragItem`).
- `tests/ui/app-test-fixture.hpp` (`AppTest`: the real application on a temporary workspace) drives it as the user
  does: `click`, `rightClick`, `middleClick`, `doubleClick`, `contextMenu`, `choose` (a combo box entry), `drag`,
  `typeText`, `hasTooltip`, `clickAt` (a point, for what has no label). The tests of the application share one
  executable (`target_sources(zalie_app_test ...)` in `tests/CMakeLists.txt`).
- Shortcut routes need one frame to register: render `frame(2)` before sending a chord.
- Requests are asynchronous: keep rendering frames until the tab is not pending any more (`waitResponse()`, bounded in
  time).
- Items without an id (a plain `Text`) are not recorded: click relative to a labeled neighbour (`clickAt`). A clipped
  item is recorded without its label: scroll it into view, or make the popup larger.
- Combo boxes do not report their label: `hoverItem` watches it and recognizes the item by the hash of the label at the
  next frame. A popup or a tab bar needs two frames after the click (`clickHere` renders them).
- Escape in a dialog whose field is active only leaves the field: click "Cancel" / "Close". `typeText` types
  everything in one frame (the URL bar takes it as a paste). Tooltips use `DelayNormal`: about 45 still frames
  (`waitTooltip`), and none while another item is active. A repeated label ("+", a collection name in the sidebar and
  in a menu) takes the index of the item.
