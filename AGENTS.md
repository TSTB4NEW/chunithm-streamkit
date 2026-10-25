# 开发约定（给后续接手的人/AI）

## 部署路径（**最容易出错的地方**）

```
bin\
├── chunithm_streamkit.asi        ← 游戏加载的是这份（bin 根目录）
└── chunithm_streamkit\           ← 插件「数据目录」，不加载任何模块
    ├── chunithm_streamkit.ini
    ├── logs\  web\  cache\  music_db.tsv
```

- **ASI 必须复制到 `bin\chunithm_streamkit.asi`**，不是 `bin\chunithm_streamkit\` 里。
  加载器和 `27inchChuni.asi` / `music_level.asi` 一样，只扫 `bin\*.asi`。
- 数据目录里的同名 ASI 是历史误放的副本，从未被加载，不要再往里放。
- 部署统一走 `deploy.cmd`（`deploy.cmd` 或 `deploy.cmd web`），不要手写 cp 路径。
- ASI 被占用时报 `Device or resource busy`，说明游戏还开着 —— 等进程退出后再部署。

## 新增游戏数据：优先 inline hook

用户给 CE（Cheat Engine）截图要求「把某个值加进来」时，**默认走 inline hook**，
不要先想着用其它字段推算（推算依赖 `note_total` 之类的推断值，容易错）。

标准流程：

1. **确认指令**：CE 里那条指令的 RVA（`chusanApp.exe+0xXXXXXX`）和原始字节。
   注意 `chusanApp.exe` 是 **PE32（32 位）**，寄存器是 `eax/ecx`；
   CE 若显示 `rcx`，那是 64 位视图，按 32 位写。
2. **hook.h / hook.cpp**：
   - 加 `kXxxWriteRva`、`InstallXxx()`、`Xxx()`、`ResetXxx()`
   - `kOrigXxx[]` 存原始字节，`InstallAt()` 会校验，不匹配就记日志并放弃
   - stub 用 `pushal → 存寄存器 → popal → 重放原指令 → jmp *back`
3. **state.h**：`Snapshot` 加字段
4. **app.cpp**：`/state` JSON 加字段
5. **profile.h**：离开对局时 `hook::ResetXxx()`
6. **main.cpp**：按 ini 的 `xxx.hook` 安装并 `L().Info` 打日志
7. **ini**：加 `xxx.hook=1` 示例
8. **web**：优先用 hook 值，做范围校验，异常时回退到旧逻辑

验证顺序：`logs\csk_hook.log` 有没有 `xxx hook: ok` → `/state` 里字段是否正确 → 前端显示。

## 修改前端后的自查

改完必须自己截图看一遍（无头 Chrome + 静态服务器预览即可），确认：
布局没塌、没重叠、颜色符合预期，再向用户汇报。

## 其它

- 前端资源在 `web\`，改完复制到 `bin\chunithm_streamkit\web\`（`deploy.cmd web`）。
- 游戏没开时 `http://127.0.0.1:8770/` 不可用，可用
  `python -m http.server 8799 --directory web` 预览静态效果（拿不到封面和实时状态）。
