# third_party

## imgui (git submodule)

[`third_party/imgui`](https://github.com/Me-Maped/imgui) 是本项目的 **git submodule**，指向
Dear ImGui 1.93.x（WIP）的 Me-Maped fork，MIT 许可（`imgui/LICENSE.txt`）。

`aimgr-gui` 用到：

| 路径 | 用途 |
|---|---|
| `imgui.cpp` / `imgui_draw.cpp` / `imgui_tables.cpp` / `imgui_widgets.cpp` | 核心 |
| `backends/imgui_impl_sdl2.cpp`、`backends/imgui_impl_opengl3.cpp` | SDL2 + OpenGL3 后端 |
| `misc/themes/imgui_themes.{h,cpp}` | 应用侧主题模块（6 套内置主题 + `.ini` 读写 + 语义化按钮） |

### 初始化 / 更新

```bash
git submodule update --init --recursive      # 首次
git submodule update --remote third_party/imgui   # 同步 fork 的最新 master
git add third_party/imgui && git commit -m "bump imgui"
```

### 不想用 submodule

```bash
cmake -S . -B build -DAIMGR_IMGUI_DIR=/path/to/any/imgui
# 或者把 third_party/imgui 目录整个删掉，cmake 会 FetchContent 上面的 fork
```

> `cmake/imgui.cmake` 会探测 `misc/themes/imgui_themes.cpp`；如果上游把主题模块挪走/改名，
> GUI 仍可编译，只是主题菜单会退化成"theme module unavailable"。
