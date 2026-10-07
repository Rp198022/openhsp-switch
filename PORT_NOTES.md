# Elona+ → Switch 移植 · 开发笔记（PORT_NOTES）

> 生成：2026-10-06（**r135 探针清理版**构建后）
> 用途：**本文自包含**——新对话/新 Agent（或换号后）直接读这一份即可接手工作，不必先读其他文档。
> 配套：桌面「Elona Switch 移植项目总结」文件夹（全纪录 + 历史文档 + 工具产物）。

---

## 0. 一句话状态

**2.30 汉化版已完整可玩（Eden 全流程 + 真机）；键位定稿 r111 已真机实测；建角「种族面板」右栏文字重叠已修复（r134），诊断探针与 P3/文件访问日志已全部清除（r135~r137，CI 成功）**。r137 已推真机，待复验。
r110b（提示文案修正："混调"→"砸开"）为 r110 与 r111 之间的过渡提交。
2.32 官方版为已可玩基线（标题 → 建角/读档 → 世界地图）。

> ✅ **已结案**：建角「种族面板」右栏文字重叠——r134 修复（判定规则 + 完整证据链见文末 **附录 G**）；`sortnote` 真实缺陷一并修掉（r131/r132）。剧情文本重叠的排查链见附录 A/E/F。
> ⚠️ **剧情文本重叠（附录 A）**：`{1}` 场景「两套坐标重绘」的问题（r104 仅拦下 CR 副本、未根治）**状态未变**，r137 后待复验。
> ⚠️ 次要：`{1}` 场景偶发"停在等待按键但渲染帧全停"（卡死）。
> ⚠️ 待查（附录 D）：中文名闪退（用户报告"又会闪退"；r110 真机日志中无此尝试，疑为 SD 旧副本或另一次运行，需向用户确认）；命名键盘"弹两次"已查明＝游戏"重名重问"机制，非 bug。

---

## 1. 项目速览

| 项 | 值 |
|---|---|
| 目标 | Elona+（HSP3 游戏）通过 **OpenHSP 运行时整体移植**跑在 Switch 大气层，产物 `.nro`；不重写游戏逻辑 |
| 路线 | ✅ A：移植 OpenHSP + SDL2/GLES 后端（B 重写否决 / C Linux 仅后备） |
| 双版本 | ① **2.32 官方原版**（Shift-JIS，基线）② **2.30 汉化版**（GBK，当前主线） |
| 引擎仓库 | `github.com/Rp198022/openhsp-switch`，分支 master，本地在 `<主工程>\openhsp\` |
| CI | GitHub Actions `Build switch NRO`（devkitpro/devkita64 容器，45-60 秒）；产物 `hsp3dish.nro` + `.elf` + `.map` |
| 构建号体系 | p3sNNN（2.32 线，至 p3s193+）→ r76~r137（2.30 线，当前 **r137**） |
| 合规 | 产物只含引擎；游戏资产用户自备，个人使用不二次分发 |

---

## 2. 当前现场（2026-10-06）

### 2.30 汉化版（主线）

| 项 | 状态 |
|---|---|
| 当前 nro | **r137（文件访问 trace 清理版）**：`_sync\build_r137\hsp3dish.nro`（9400376 B，MD5 `27f81043efe630f95c4747880ea9c3f4`，CI `Build switch NRO` 成功）；r136（9404472 B）在 `_sync\build_r136\`；Eden SD 卡上的 nro 仍是 r134（被运行中的 Eden 锁定） |
| Eden 验证 | ✅ 完整跑通：标题 → 角色创建全流程 → 名字输入 → 开场 → **世界地图**（中文正常、帧率稳定）；黑边提示构建正常（日志 `overlay built L160x214 R160x500`） |
| 真机验证 | ✅ **r110 nxlink 推送成功**（2026-10-06 00:26）：手柄识别正常、按键注入正常（pushed 892 次）、正常退出（err=0）；用户实测发现 X/Y 交叉（r111 已修，Eden 复验 V→`z`、Z→`x` 通过）；⏳ r111 推送待用户；**r135/r136/r137 已依次 nxlink 推送（10-06 16:04 / 16:2x / 16:3x），当前真机运行 r137，全部秒级启动、stdout 回连正常** |
| 黑边键位提示 | ✅ 实装（r105~r110b）：左黑边=左手柄键位、右黑边=右手柄（含第二层）；**R+右摇杆按下** 切换显示/隐藏；布局随窗口自动重建 |
| 键位定稿 | **r111**：基础层 X=行动菜单、Y=道具菜单、左摇杆按=阅读；**R 层新增左摇杆五键**（跳跃`h`/挖掘`D`/给予`G`/关闭`C`/切换弹药`A`）；右摇杆 R 层=丢弃/打开/日志/砸开；R+L=删存档；全表见 §11 与 `历史文档\switch_keymap_design.md` |
| 运行目录 | `<2.30工作区>\_hcrun\`（`elonaplus2.30\` 数据 + `hsp3dish_boot.log` + `charset.txt` + `gbk.tbl` + `ipaexg.ttf`） |

### 2.32（基线，与 2.30 共享移植层）

- Eden：可玩（移动/滚动重绘/径向菜单/文本输入含回车提交/玩家贴图正确）。
- 真机：第 21 轮键位表已部署（SHA256 读回一致）；贴图/输入修复的真机复验未做。

---

## 3. 下一步任务（按优先级）

0. ✅ **（已完成）种族面板右栏重叠**：r134 修复（技能列 x 286~298 / y 385~520 内「无连续两个半角空格」的行 = 说明副本 → **只跳过 blit**）；r135 清除全部探针；用户确认画面正常。详见 **附录 G**。
1. **r111 推送 + 键位手感反馈收集**（向用户）：
   - r111 nro 在 `_dl\r111\`（推送方式见 §5.3）；**推送前先问用户**；
   - 收集点：X/Y 修正后的手感、R+左摇杆五键（跳跃 `h`/挖掘 `D`/给予 `G`/关闭 `C`/切换弹药 `A`）、R+右摇杆四向、R+L 删存档、黑边提示显示效果；想换键位改 `SW_LSTICK_*_ALT` / `SW_RSTICK_*_ALT`（一行一处）。
2. **中文名闪退待查**（见附录 D）：向用户确认「当时是 nxlink 推送版还是 SD 卡旧版启动」「崩溃发生在哪一步」；必要时 nxlink `-s` 现场抓复现日志。注意：≤r98 旧版本就有中文名存档崩溃（r99 修复）——若当时跑的是 SD 旧副本即可结案。
3. **真机全流程复验**：中文显示（GBK 全字符）、角色创建全流程、世界地图、帧率与手感。
4. **把 2.30 线（r83~r111）变更补写主移交文档**：`ELONA_SWITCH_MASTER_HANDOVER.md`（v1.13）停在第 19 轮、`.new.md` 停在第 29 轮。按 MASTER §0 维护规则追加为第 30+ 轮记录（含键位/黑边提示/文本重叠调查/命名机制）。
5. 2.30 遗留深挖：汉化版命令集（HSP3.50 编译）是否有未触发调用点；`≒` 缺字（可忽略）。
6. 2.32 遗留：BGM（TiMidity / 转 OGG）；"世界地图遇敌闪退"复现；战斗/存读档切片；R33/R34/R35。
7. 发布前清理：✅ 探针 `t23~t38`（r135）、`SWITCH_DIAG` 置 0（r135）、`dllshim_switch.cpp` 无条件 P3 日志（r136）、`glue_switch.cpp` 的 `hsp3file:` 逐次文件访问 trace（r137）均已清；⏳ 仍剩 `hsp3text:` 文本注册 trace、`hsp3screen:` 失败告警与 overlay 构建日志降噪（如需再清说一声）。

---

## 4. 关键路径速查

| 代号 | 路径 |
|---|---|
| 主工程 | `C:\Users\rao\AppData\Roaming\TRAE SOLO CN\ModularData\ai-agent\work-mode-projects\6ab7990a9faeaf1a74bc3bed` |
| ↳ git 仓库 | `…\6ab7990a9faeaf1a74bc3bed\openhsp\`（makefile.switch、`src/hsp3dish/switch/`、`src/hsp3/switch/`） |
| ↳ CI 产物 | `…\6ab7990a9faeaf1a74bc3bed\_stage\build\<tag>\`；本次 r110b 在 `…\openhsp\_dl\r110b\` |
| ↳ Eden | `…\6ab7990a9faeaf1a74bc3bed\_stage\emulator\eden\eden.exe` |
| 2.30 工作区 | `…\work-mode-projects\6ac27df8b851cd8e44ed73ee`（r9x/r11x nro、zz230_* 脚本、deploy 脚本、`r110_nxlink.log`） |
| ↳ 运行目录 | `…\6ac27df8b851cd8e44ed73ee\_hcrun\` |
| ↳ 本次真机日志 | `…\6ac27df8b851cd8e44ed73ee\r110_nxlink.log`（nxlink stdout 实时抓取） |
| 对照源码 | `…\work-mode-projects\6abe770cb851cd8e44ed2318\elona232src\`（41 个 .hsp，ElonaPlus 2.32） |
| 主移交文档 | `…\6ab7990a9faeaf1a74bc3bed\ELONA_SWITCH_MASTER_HANDOVER.md`（详细）+ `…\6abf034db851cd8e44ed315d\ELONA_SWITCH_MASTER_HANDOVER.new.md`（精简） |
| 桌面总结 | `C:\Users\rao\Desktop\Elona Switch 移植项目总结\`（全纪录 + 本文 + 历史文档 + r110b/r98 nro + nxlink.exe） |

**可改范围**：`openhsp\src\hsp3dish\switch\*`、`openhsp\src\hsp3\switch\*`、构建/CI 文件。**不可改**：上游非 Switch 区域。

---

## 5. 构建与部署

### 5.1 一轮循环（CI → 部署）

```powershell
gh run list --repo Rp198022/openhsp-switch --limit 3        # 找 run id
gh run watch <id> --repo Rp198022/openhsp-switch --exit-status
gh run download <id> --repo Rp198022/openhsp-switch --name hsp3dish-nro-only -D <dir>
# 关 Eden → 覆盖 SD 的 switch/openhsp/hsp3dish.nro → 重启
```
- `git push` 已可用（2026-10-03 起）；若又被墙，退回 GitHub Contents API（`t23_api_commit_multi.py` 模板）。
- 仓库行尾不统一（LF/CRLF 混合），回写文件必须保持原样。
- 提交消息**用单行**（PowerShell 里多行/嵌套引号会解析失败，见 §9）。

### 5.2 Eden 模拟器（主验证通道）

- 启动：`& "<eden.exe>" "<nro路径>"`（**必须传 .nro 文件**，传目录不生效）。
- 键位：键盘 `C`→Switch A（确认）/ `X`→B（取消）/ `V`→X / `Z`→Y / `Q`→L / `E`→R / `M`→+ / `N`→-；方向键→十字键；**鼠标点击=触摸屏**。
- 坑：窗口 resize 会卡死（0 FPS，重启 Eden）；Eden 监视游戏目录，目录内写入风暴会崩 → dump 写 `sdmc:/dump/`；`keys.txt` 必须无 BOM 写入；宿主机直接写游戏目录的 keys.txt 会崩（用硬链接）。

### 5.3 真机（Tegra X1）

- **必须按住 R 用"应用模式"进 hbmenu**（相册/applet 模式内存不足，0x505，屏幕 8 无渲染目标）。
- 三条部署通道：
  1. **nxlink**（首选，实测可用）：hbmenu 按 Y 进 netloader → 预检 `Test-NetConnection <IP> -Port 28280`（True=已在等）→ `nxlink.exe -a <IP> -s <nro>`；
  2. **DBI FTP**：不重启写真实 SD（`ftp://<IP>:5000/` anonymous）——补文件首选；
  3. MTP：Shell.Application 遍历 `Switch → SD Card → switch`，`CopyHere`。
- 真机应用是 `switch/hsp3dish.nro`；数据在 `switch/openhsp/`。
- **本次实测参数（10-06）**：Switch `192.168.5.7`、PC `192.168.5.5`；netloader 端口 `28280`；推送后 Switch 回连 PC 的 stdout 端口 `28771`（`netstat` 可见 ESTABLISHED）。完整记录见 **附录 C**。

---

## 6. 运行期证据与日志

| 文件 | 位置 | 说明 |
|---|---|---|
| `hsp3dish_boot.log` | 游戏目录（真机=SD 卡） | stdout（cp932，每次运行截断）；排查第一现场 |
| `hsp3dish_diag.log` | 游戏目录 | FBO attach/buffer/texload（追加累积，**读前先删**，否则误判） |
| `smp_*.bmp` / `sw2_*.bmp` | `sdmc:/dump/` | 各屏 FBO 回读（必须写监视目录之外） |
| **`r110_nxlink.log`** | 2.30 工作区根目录 | **本次真机 nxlink stdout 实时抓取**（530KB+，游戏运行中持续增长；含 overlay 构建行/手柄识别/按键注入证据） |
| 历史真机日志 | `…\_stage\vsd\switch\openhsp\hsp3dish_boot*.log` | 早期真机回读样本 |

**日志快速判据**：GL 0x506 = 上下文重建/对象失效；`rebuild_window` 出现次数 >1 = 需 `hgio_resume()`（r97 修复）；`hsp3file: FAIL` 连续 = 缺文件/路径问题；`overlay built L…R…` = 黑边提示已构建（数字=纹理尺寸，随窗口布局自动变）；`overlay hidden/shown` = 提示开关被触发。

---

## 7. 硬约束（勿违反）

1. **回复语言：中文。**
2. 项目目录对 Write/Edit 工具受限（主工程尤其）：改源码走「补丁脚本 → `wsl python3` 执行」，补丁用**行扫描 + 唯一锚点**（整块字符串匹配常因 CRLF 失败）。桌面文档可先写到工作区再用 `Copy-Item` 铺过去。
3. PowerShell 里 `| head` 不可用（用 `Select-Object -First/-Last`）；`wsl -e bash -c '...'` 带空格路径注意引号。
4. 模拟器/游戏跑副屏，**不要动用户正在玩的游戏**；截图工具不抢焦点（用 `t23_shot2.ps1` 类）。**用户正在真机游玩时不要推送/重启**，先问。
5. 每轮结束按 MASTER §0 规则更新主移交文档（追加轮次记录；不要新建平行 handoff）。
6. 桌面文件夹是归档，**工作现场在主工程目录**；桌面「总结」文件夹的当前入口是 `AGENT_交接文档_20261006.md`（本文）。

---

## 8. 勿回退清单（关键修复，均有证据）

| 修复 | 一句话 |
|---|---|
| 主屏独立 FBO + 每帧 `sw_main_present()` | 修标题闪烁/背景丢失（不要改回直画窗口） |
| `CLSMODE` 清屏后复位 NONE | 否则每帧清屏破坏累积式重画 |
| `sw_texture_storage.allocated` 逐纹理校验 | OOM 纹理挂载会硬崩 2168-0002 |
| 纹理绑定只走 `sw_bind_tex()` | `ChangeTex` 缓存 + 两套 id 空间（TEXINF 下标≠GL 名）是最高频坑 |
| `hgio_setTexBlendMode` 用于一切新绘制/上传路径 | ES2 纹理 incomplete 会采样不透明黑（R37） |
| `z.hpi` 真实现（stdio + zlib gz*） | 地图/存档全走它，gzip 流 |
| 字符注入通路 `switch_input_take_keys()` | Elona 字母/回车只来自隐藏 keylog 框 |
| `glFinish()` 在 `sw_fcgraph_sub` 减色路径（含零减色早返回分支） | PCC 贴图 GPU 竞态 |
| `NOTICE_KEY_CR` → `addStringFromCaret("\r\n")` | 输入框回车结束不了 |
| **`hsp3excmd_rebuild_window()` 里的 `hgio_resume()`** | r97：2.30 第三次重建窗口 GL 失效 → 黑屏 |
| **`sw_path_to_utf8()` 剥离 0x01-0x1F** | r98：路径含回车 → 存档文件夹建不出 |
| **overlay 绘制前 `glEnable(GL_TEXTURE_2D)` 并保存/恢复** | r105b：ChangeTex(-1) 关掉纹理开关 → 面板整块纯白 |
| **肩键定案：L=Tab（切标签）、R=第二层** | r106 用户互换后定稿；R+L 删存档依赖此布局 |
| **面板列宽动态测量 + 各自贴边对齐** | r108/r109：固定偏移会重叠/挤压；测量后才不越界 |
| **键位定稿 r110**：X=`z` 行动菜单、R+X=`g` 拾取、左摇杆按=`r` 阅读、右摇杆第二层 `d/o/j/b` | 用户逐项确认；改键前先问用户 |
| **r111 X/Y 修正 + 左摇杆第二层**：上键=`SDL_CONTROLLER_BUTTON_Y`、左键=`SDL_CONTROLLER_BUTTON_X`；R+左摇杆 `h/D/G/C/A` | SDL 按 Xbox 位置命名与任天堂标签交叉（Eden 实测 V→`z`、Z→`x`、R+F→`A`）；勿改回标签直连 |
| **r131/r132 `sortnote` 真实现（带界 + 空/非字符串跳过）** | r93 的 stub 让 `sortget` 取到陈旧源行；恢复后原实现遇**空 note** 抛 `err=3`（游戏从 `userNpc_update` 调用）→ 必须宽松跳过 |
| **r134 技能列错行「只跳过 blit」（`hgio_mes`）** | r133 在 `hgio_mes` 提前 `return` 会**卡死启动**；只跳过 `hgio_fontcopy`，前置步骤（clean/居中/贴图注册）全保留 |

---

## 9. 常见坑速查

- SDL 按 Xbox 位置命名 Switch 按键（A/B、X/Y 交叉过，按物理位置映射）。
- `hsp3dish_modname()` 必须收模块文件路径而非目录（换目录部署复发）。
- `msgfunc` 不是逐帧回调；帧边界用 `hgio_render_start()`。
- `swExpand` 扩容表不清零（同类表仍可能踩）。
- `gzoom` 前两参是目标尺寸；`exist` 是命令（结果进 `strsize`）。
- 每 draw ≈5.3µs、60fps 约 3100 draws 预算——卡顿主因通常不在渲染代码，在脚本 CPU/文件系统热路径。
- `#if SWITCH_DIAG` 包整块 `{...}` 会切断括号平衡（只改表达式）。
- **组合键必须静默被借用的基础键**：R+右摇杆按 切换提示时，Space（休息）与 R 层的 Tab 都要闭嘴；R+L 删存档时 Tab 也要闭嘴（一键两报 = R48/R49 教训）。
- **nxlink 输出经管道重定向时是 C stdio 块缓冲**（4KB 才 flush）——抓日志看不到实时进度是正常的，用 `netstat` 看连接状态、或等游戏启动后一次看全量。
- **PowerShell 提交消息用单行**，多行/内嵌引号会被拆成 pathspec 报错。
- overlay 在无字体时整体禁用（日志 `overlay: no usable font`）；真机字体 = `sdmc:/switch/openhsp/ipaexg.ttf`。
- 桌面/工程目录外写文件用 `Copy-Item`（Write/Edit 工具限于工作目录内）。

---

## 10. Eden 角色创建操作序列（2.30 汉化版实测，可直接自动化）

1. 标题菜单：Down×1 → C（Generate an Adventurer）
2. Alias：Down×1 → C
3. Profile：C×2（Proceed）
4. 名字确认对话框：Down×1 → C（No）；穿插 C×2~3
5. Gender：C；Class：C → 名字确认 Down×1→C
6. Skill：Down×1 → C
7. Attb：C → Feats 面板：Down×2→C 取得特性（重复至用完）→ Down×12 到底 + C 前进（X=关闭面板）
8. Appearance：C（Done）；Balance：C（Essential）
9. 总览：**X** 呼出 "Are you satisfied now?" → C（Yes）
10. 名字输入：敲字母 → **鼠标点击软键盘绿色 OK 按钮**（触摸屏）
11. 开场：连续 C ×N → 世界地图

**判据**：日志 `clrobj ... objsel` = 输入提交成功；SD 出现 `save\sav_*` = 角色已保存（入世）。

---

## 11. 当前键位表（r110b 实装版，详表见 `历史文档\switch_keymap_design.md`）

**基础层**：十字键=移动/光标；左摇杆↑↓=下楼/上楼；左摇杆←=角色情报；左摇杆→=投掷；**左摇杆按=阅读**；**X=行动菜单**；Y=道具菜单；A=确认/攻击；B=取消；L=切换标签；**R=第二层（按住）**；ZL=射击；ZR=咏唱；−=锁定目标；＋=存档/设置；右摇杆↑↓←→=挥杖/搜索/特技/对话；右摇杆按=原地休息。

**第二层（R+）**：A=`e` 吃；B=`q` 喝；**X=`g` 拾取**；Y=`w` 装备；−=`p` 祈祷（情报界面=外观编辑）；**左摇杆↑=`h` 跳跃；左摇杆↓=`D` 挖掘；左摇杆←=`G` 给予；左摇杆→=`C` 关闭；左摇杆按=`A` 切换弹药**；右摇杆↑=`d` 丢弃；右摇杆↓=`o` 打开；右摇杆←=`j` 日志；右摇杆→=`b` 砸开；**R+L=删除存档**（存档列表）；**R+右摇杆按=显示/隐藏黑边提示**。

---

## 12. 文档索引

| 文档 | 位置 | 用途 |
|---|---|---|
| **本文** | 桌面\ + `_summary\` | Agent 交接（最新状态） |
| `Elona_Switch_移植项目全纪录.md` | 桌面\ | 人读全时间线（09-26→10-06） |
| `历史文档\switch_keymap_design.md` | 桌面\历史文档\ | **手柄键位映射（r110b 定稿实装版 + 变更历史）** |
| `历史文档\AGENT_交接文档_20261005.md` | 桌面\历史文档\ | 上一版交接（r98~r104 状态存档） |
| `历史文档\ELONA_SWITCH_MASTER_HANDOVER.md` | 桌面\历史文档\ | 主移交 v1.13（第 1-19 轮详细） |
| `历史文档\ELONA_SWITCH_MASTER_HANDOVER.new.md` | 桌面\历史文档\ | 精简版（第 20-29 轮） |
| `历史文档\round27.md` | 桌面\历史文档\ | PCC 竞态修复 + Eden 环境坑细节 |
| `工具与产物\hsp3dish_r110b.nro` | 桌面\工具与产物\ | **2.30 现用 nro（最新，键位定稿+提示功能）** |
| `工具与产物\hsp3dish_r98.nro` | 桌面\工具与产物\ | 2.30 旧版 nro（回退用） |
| `工具与产物\r110_真机日志.txt` | 桌面\工具与产物\ | 真机 nxlink 日志样本（r110 推送） |
| `工具与产物\nxlink.exe` | 桌面\工具与产物\ | 真机推送工具 |

---

## 附录 A：剧情文本重叠问题调查记录（2026-10-05 · 进行中）

> 状态：**未根治**。r104 拦截了"CR 副本"（9 宫格描边版全部被跳过），但用户复测仍见重叠；
> 主因已定位为"同一文本被两套坐标系统同帧同时绘制"。以下为可复用的完整证据链。
> （10-06 状态：本日集中做键位/体验/文档，本问题未推进；接手优先级高。）

### A.1 现象
- 2.30 汉化版剧情场景（韦尔尼斯士兵剧情）文本出现重影/叠加。
- 典型："在仔细的盘问后，士兵们注意到了你诧异的眼神，" 同帧在 y=247 与 y=280 各绘一份（差 33px）。

### A.2 探针体系（已内置，勿删，发布前再清）
| 探针 | 位置 | 内容 |
|---|---|---|
| t24 | `hgiox_switch.cpp` hgio_mes | 每次文本绘制：bm/tx/type/x/y/tick/字符串（仅 CJK；同位置 1s 去重） |
| t25 | `glue_switch.cpp` texmesRegist wrap | 每次"新字符串注册"：cs（脚本位置）/tick/GBK 字符串 |
| t26 | `hgiox_switch.cpp` hgio_mes（r104） | 被跳过的 CR 副本绘制（上限 300 条） |

### A.3 已查明事实（含字节级证据）
1. **同一 tick（592930）内"盘问"注册两次**（t25 #699/#700），两次 cs 均为 **95168**（同一脚本代码点）。
2. **第二次字符串尾部多 1 字节 0x0D(CR)**（日志原始字节验证：#699 以 `A3 AC` 结尾；#700 以 `A3 AC 0D` 结尾）。
   - `texmesRegist` 以 `strcmp` 比对内容 → "A" ≠ "A\r" → 两次建纹理、两次绘制（y=247 / y=280）。
3. **每个文本每帧绘制 9 次（3×3 网格，x±1 / y±1）** = 袋文字描边（bmes 9），属正常渲染行为。
4. **两套坐标系统同绘一段文本（重叠主因）**：
   - 版1：**居中、20px 行距**。实测坐标：'他们告诉你'@(248,267)、'街边'@(312,307)、'患有'@(240,327)、'但每'@(280,347)。
   - 版2：**左对齐 x≈223、16.67px 行距**。实测：'盘问'@(224,248)、'他们'@(224,297)、'街边'@(224,330)、'患有'@(224,347)、'但每'@(224,364)。
   - 两版换算关系（r102 曾对 7 行全部验证）：**y2 = floor((y1+90)×5/6)**；5/6 = 600/720 →
     疑似"按 720 高度设计的排版层被缩到 600 画布"。
   - 同文本两版 y 仅相距 ~30-35px（显示缩放后 ~35-42px）→ 视觉重叠。
5. 早期版本观察补充：x 方向另有"居中公式 x = 400 - 文本宽/2"痕迹（见"却兵"x=271-273）。

### A.4 修复尝试记录
| 版本 | 思路 | 结果 |
|---|---|---|
| r103 | 跳过"空行后的重复行" | ❌ 未命中（实际序列中两份之间没有注册空行） |
| r104 | 跳过"尾部带 CR 且 5ms 内同 x、Δy∈[6,48] 已画过同文本"的绘制（仍推进 cy） | ✅ CR 副本 9 宫格全部被拦（t26 十条记录）；⚠️ 用户复测仍重叠（两套坐标系仍在绘制） |

### A.5 下一步建议（接手直接可用）
1. **给 t24 增加 cs 字段**（`code_getpcbak()`，与 t25 同源）→ 一次性区分"两套坐标"各自的脚本调用点。
2. **对照实验**：2.32 基线版在 Eden 是否同样出现两套？（判断是"2.30 汉化特有"还是"移植通病"）。
3. **查"720 排版层"启用条件**：screen/ginfo 汇报值、对话框模块分支（可对照 `elona232src` 41 个 .hsp 搜相关判断）。
4. **修复候选**：
   - 让脚本读取的屏幕尺寸与画布一致（800×600）；或
   - 在移植层对齐两套坐标（参考 `hgiox_switch.cpp` 注释 "The window is 1280x720 while the script draws an 800x600 canvas"）。

### A.6 关键引用
- 提交（仓库 `Rp198022/openhsp-switch`）：r100 `ebedeb7` / r101 `5f2edbd` / r102 `5eb4abe` / r103 `e43bfc1` / r103b `baaeba6` / r104 `7af8dbb`。
- 日志：`sdmc/switch/openhsp/hsp3dish_boot.log`（GBK 编码；含 t24/t25/t26 探针；读得用 936 解码或 Latin-1 二进制比对）。
- 复现路径：读档 → 出世界地图 → 进韦尔尼斯（士兵剧情自动触发；日志时间 t≈99s 段落）。
- 依凭稿：本问题首份集中记录见 `AGENT_交接文档_20261005.md` 附录 A。

---

## 附录 B：左右黑边键位提示（overlay）实现记录（10-05 晚 ~ 10-06，r105~r110b）

### B.1 需求（用户给定）
- Switch 是 16:9，Elona 画布 4:3 → 左右各 ~160px 黑边（1280×720 时）。用户要求**把黑边利用起来显示手柄键位**：
  左黑边=左手柄键位、右黑边=右手柄键位（含第二层）；**R+右摇杆按下** 隐藏/显示；文字贴边、清晰、不重叠。

### B.2 实现
- 新模块 `switch_overlay.{h,cpp}`：SDL_ttf 把整块提示渲染成**静态 RGBA 纹理**（左右各一张，只建一次），每帧只画 2 个 textured quad（性能零负担）。
- 挂接：`hgiox_switch.cpp` 的 present 阶段（窗口帧缓冲已绑定、Swap 之前）调用 `switch_overlay_draw(win_w, win_h, origin_x, game_w)`。
- 布局：**动态测量两列文字宽度**，左栏左对齐贴左缘、右栏右对齐贴右缘（边框留白 2px）；黑边宽变化（docked/undocked）时自动重建。
- 字体：游戏自带 `sdmc:/switch/openhsp/ipaexg.ttf`（中文可用），16px。
- 切换：`switch_input.cpp` 里 R+右摇杆按 边沿触发 `switch_overlay_toggle()`；同时静默 Space（休息）避免一键两报。
- makefile：`OBJS_DISH` 登记 `switch_overlay.swd`。

### B.3 迭代与坑（r105~r109）
| 版本 | 问题 → 修复 |
|---|---|
| r105 | 初版上线 |
| r105b | **整块纯白**：`ChangeTex(-1)` 关掉 GL_TEXTURE_2D → shader 采样默认白 → 绘制前 `glEnable(GL_TEXTURE_2D)` 并保存/恢复（`sw_texture2d_on()` 查询原态） |
| r105c/d | 提清晰度：去字体描边（显糊）、NEAREST 滤镜、亮度调灰阶、−/+ 缺字形改"减号/加号"汉字 |
| r106 | **L/R 功能互换定案**（L=Tab 切标签、R=第二层）+ 字号加大到 16px |
| r107/r108 | 列位贴合屏幕边缘：先 5px 外移，后改为**动态测量列宽**（固定偏移会导致标题挤压键位列） |
| r109 | 标题单独靠外边距（最宽的组标题不再挤压键位列）；边距收紧 2px、列距 4px、右栏再外移 14px（最大化贴边） |
| r110 | 内容更新：X=行动菜单、R+X=拾取、左摇杆按=阅读、第二层右摇杆四向、R+L 删存档 |
| r110b | 文案修正：第二层右摇杆→"混调"→"砸开"（`b`=key_bash） |

### B.4 提交
`b17a9e1`(r105) → `3232b7c`(r105b) → `4d499f3`(r105c) → `ad7e937`(r105d) → `50592ff`(r106) → `95a98e8`(r107) → `737c838`(r108) → `8e16a23`(r109) → `164c7eb`(r110) → `f2a8873`(r110b) → `fe142a3`(r111)

### B.5 真机验证（10-06 r110）
日志实证：`overlay font sdmc:/switch/openhsp/ipaexg.ttf` → `overlay built L160x214 R160x500`（加载后再随布局重建一次 L159/R161）→ 无 GL 错误；用户画面上可见左右提示。

---

## 附录 C：真机 nxlink 推送记录（2026-10-06）

- **前置**：Switch 已按住 R 应用模式启动 → hbmenu 按 Y 进入 netloader（等待中）。
- **预检**：`Test-NetConnection 192.168.5.7 -Port 28280` → True（netloader 在听）。
- **推送命令**（PC 端，2.30 工作区）：
  ```powershell
  & "<...>\nxlink.exe" -a 192.168.5.7 -s "<...>\openhsp\_dl\r110\hsp3dish.nro" 2>&1 | Out-File r110_nxlink.log -Encoding utf8
  ```
- **证据链**：
  1. `netstat` 先见 `PC:xxxxx → 192.168.5.7:28280 ESTABLISHED`（上传中），后变 TIME_WAIT（传完）；
  2. 出现 `PC:28771 ← 192.168.5.7:xxxxx ESTABLISHED` = 游戏侧 stdout 回连（libnx nxlinkStdio），说明**应用已启动**；
  3. `r110_nxlink.log` 中：`hsp3switch: game controller = Switch Controller`、`overlay built L160x214 R160x500`、`PADRAW ... pushed=104`（用户按键被正常注入）、游戏内真实存档读写（`save/sav_2/...`）——**推送成功、游戏正常运行**。
- **注意**：`Out-File` 管道重定向时 nxlink 的 stdout 是块缓冲（4KB），推送过程中日志文件可能长时间为 0 字节，用 `netstat` 判进度；游戏侧 SD 卡 `hsp3dish_boot.log` 始终是第一现场（可事后 FTP 取回）。
- **r110b 推送待办**：命令同上，换 nro 路径为 `_dl\r110b\hsp3dish.nro`；**推送前先征得用户同意**（其正在游玩）。

---

---

## 附录 D：命名界面的两问与"中文名闪退"待查（2026-10-06）

### D.1 "输入法弹出两次" = 游戏的"重名重问"机制（非 bug）
r110 真机日志（`r110_nxlink.log` 第 7245~7269 行）完整记录了一次命名：
- 第一次弹出系统键盘，用户输入 **"1"** → 提交 → 游戏画出提示（日志字节级还原为 **"那个名字的冒险者已经存在了。"**）→ **再次弹出键盘** → 用户输入 **"2"** → 通过（随后进入开场，自动存档创建 `save/sav_2`）。
- 依据：`chara.hsp:4480 *cm_name_loop`——`playerid = "sav_" + cmname` 后扫描 `save/*`，若重名则提示并 `goto *cm_name_loop` 重问。**SD 卡上已存在 `sav_1`**（此前测试遗留），故 "1" 被判重名。
- 结论：键盘弹两次是**游戏设计**（重名→重问）；选未用过的名字只会问一次。建议清理 SD 上遗留的测试存档。
- 附：日志中文还原方法——GBK 文本经管道变 U+FFFD 后不可逆，但**特征字符**（"已经存在了"→`Ѿ`、`ˡ`）可与游戏文本对回原文。

### D.2 X/Y 交叉的根因（r111 修复）
- SDL 对 Switch 手柄**按 Xbox 位置**命名正面键：`SDL X`=左侧（任天堂 Y）、`SDL Y`=上方（任天堂 X）。
- r110 及更早按标签直连 → 按上键出 `x`（道具菜单）、按左键出 `z`（行动菜单）——即用户反馈的"X/Y 还是反的"。
- r111 交换映射；Eden 实测复验：Eden `V`(=Switch X)→注入 `z`、`Z`(=Switch Y)→注入 `x`；`R+F`(=左摇杆按下)→`A`（切换弹药）亦通过。
- 参考 SDL 源码 `SDL_hidapi_switch.c` 的 `RemapButton()`：labels 关闭时 A↔B、X↔Y 位置化重映射。

### D.3 中文名闪退（未复现，待用户信息）
- r110 真机日志中**没有**中文名尝试（命名区间只有 "1"/"2" 两次数字输入），说明崩溃发生在**另一次运行**。
- 已知：≤r98 版本存在"中文名存档核心文件缺失→闪退"（r99 修复：zOpen/RemoveDirectoryA/delfile 补 UTF-8 转码）；r110+ 源码已含 r99 修复。
- **待确认**：① 崩溃那次是从 hbmenu 启动的 SD 旧副本，还是 nxlink 推送的 r110？② 崩溃时机（输入确认时/开场时/入世自动存档时）。
- 复现方案：nxlink `-s` 推送 r111 并抓 stdout，让用户复现一次。

---

*接手第一件事：读 §0 状态 → 若用户已给键位反馈则处理 → 否则按 §3 推进入口（文本重叠 附录 A / r111 推送）。*

---

## 附录 E：种族面板文字重叠的排查（2026-10-06；**已结案，结局见附录 G**）

### E.1 现象
建角「选择我的种族」界面（**仅**艾沃达纳人/妖精/丘陵人/朱伊安人/巫师/魔像 这 6 个种族）：
右侧面板的**文字错位/重叠**——说明文字与"初始技能"栏互相压住。

### E.2 已修复的部分（r121 已真机验证）
- **换行缓冲到达绘制层时含内嵌换行**（r120 抓到一段 126 字节含 CRLF 的"两行合一"段）：
  `hgio_mes` 因此只画到换行处、且只推进一行 → 说明栏少一行、下面整体上移。
- **修复**：`hgio_mes` 自己按 CR/LF 拆行逐行绘制（[hgiox_switch.cpp](/..)），真机验证说明栏 5~6 行完整、行距正确。
- 附带修复：r117（文字贴图缓存满时回收最旧一条，而不是静默丢行）。

### E.3 仍存在的问题（尚未修）
技能栏那一列（x≈292/295）**额外画了"消息类"文字**，与技能行重叠：

| 内容 | x,y | 特征 |
|---|---|---|
| 个人经历（背景故事） | 295, 304/319/334/349/364 | **5 次独立单行 mes**（brk=0），**行距 15px** |
| 种族说明的片段 | 292, 393/423/439 | 单行调用，与技能行同列 |

**探针证据（真机，UTF-8 日志 + 十六进制输出）**：
- `t31hex`（技能列每次绘制的原始字符串，hex）：确认技能行本身正常
  （`292,364 武器专精  格斗,枪械,投掷` / `378 读书 …` / `392 交涉 …`），
  同时确认额外的、内容为"经历/说明"的行也在同一列被绘制。
- `t32hex`（`PrintSub` 入口的**整条** mes 参数 + 换行数）：那 5 行经历**全是单行调用**
  （`brk=0`）、位置由脚本给出（304→319→334→349→364，步长 15）。
- `t32pc`（脚本代码位置，复用 `code_getpcbak()`）：5 行的 cs 为
  **1864406 / 1864445 / 1864484 / 1864523 / 1864562**，**每行相差正好 39 字节**
  → 同一段**循环体**逐行绘制。

### E.4 已排除的路径（都有真机证据）
| 怀疑 | 结论 |
|---|---|
| 换行函数 `talk_conv` 丢字 | 否（t27：缓冲完整 246 字节 4 行） |
| 文字贴图缓存满 → 丢行 | 是缺陷但非本例（r117 已修，修后本现象仍在） |
| 字体度量 `sx==0` | 否（t28 零命中） |
| `hgio_mes` 的 4 个静默返回 | 否（t29 零命中） |
| 贴图取错（缓存哈希撞车） | 否（`texmesGetCache` 有 strncmp 全串比对） |
| `notesel/noteget` 行表残留 | 否（`CStrNote::Select` 无状态，按需扫描） |
| hspda 的 `xnotesel/xnoteadd` | 否（真机日志 0 次调用） |

### E.5 结论与下一步
这批文字是**游戏自己按"当前文本游标"逐行绘制**的消息/经历文本（15px 行距 = 消息窗口节奏），
在原版里游标在消息区域，我们的移植版里游标停在**面板的技能列**上 → 叠进面板；只有说明较长的
6 个种族会让面板画得更低、正好露出这段。

**下一步（二选一）**：
1. **对照 2.32 官方版**：引擎 nro 与游戏数据分离，可直接用同一支探针版 nro 跑
   `6ab7493c/assets/official232/elonaplus2.32`（422MB）的同一界面。
   2.32 也出现 → 移植层通病；不出现 → 与汉化版文本/度量相关。**（需先把 Eden SD 上的
   2.30 数据备份、换成 2.32，约 1GB 搬动）**
2. **探消息绘制入口**：在 `msg_write`/消息窗口那条路的坐标来源处加探针
   （看它期望的坐标来自哪个 `inf_msg*`/`pos`），确认游标差异从哪来。

**当前真机版本**：r126（= r121 修复 + t31/t32/t32pc 探针）。探针 t31~t32 在定位后需撤掉。

### E.6 关键日志/脚本
- 抓取脚本：`_sync/scan_t31hex.py`、`_sync/scan_t32pc.py`、`_sync/scan_t27.py` 等
- 真机日志：`r124_nxlink.log`（t31hex）、`r125_nxlink.log`（t32hex）、`r126_nxlink.log`（t32pc）
- 提醒：真机 stdout 日志里**非 ASCII 字节会被替换成 U+FFFD**，探针要打**十六进制**才可还原。

---

## 附录 F：种族说明"叠到初始技能"的行来源定位（2026-10-06，r127/r128；**结局见附录 G**）

### F.1 结论（已用带 cs 的探针确认）
不是文字渲染问题，而是**同一个"行"被两段不同脚本各画了一次**：

| 绘制 | x,y | cs（脚本位置） |
|---|---|---|
| 说明的正常绘制 | x=270, y=132/148/164/180/196（步长16） | **1875545** |
| 属性行（力量/感知/魔力…） | x=292, y=286/302 | 1876153 |
| 技能列表 | x=292, y=364/378/392/406/420（步长14） | 1876373（首行）/ **1876593**（其余） |
| **说明的"副本"** | x=292, y=393/408/423/439…（步长15/16） | **1876593 = 技能行的 cs** |

**副本的 cs 与技能行完全相同** → 说明文本是被"技能列表绘制"那段代码画出来的，
而不是独立的说明绘制。且副本内容 = 说明字符串的**尾部若干行**（见 F.3）。

### F.2 数据段事实（start.ax）
- `HSPHED.pt_cs = 112` → 代码段文件偏移 = 112 + cs。
- 技能描述在文件偏移 `11857238` 附近（"令你能读懂难以理解的书籍"@11857238）。
- **种族说明是"一整条连续长字符串"（无换行）**，位于 `13764738` 附近，例如：
  - `eulderna / 艾沃达纳人` @13764709-13764729，说明正文 @**13764738**（长度 241 字节）
  - `juere / 朱伊安人` @13766656-13766671，说明正文 @**13766677**
  - 说明按种族顺序连续排列，每条后跟英文说明。
- **换行是游戏运行时按固定字节数切出来的**（每行 31 汉字 = 62 字节）。
- 关键校验：艾沃达纳说明里"他们来统治。"位于 `13764862` = `13764738 + 124`，
  而真机上副本正是从该句开始 → **副本从说明的第 3 行起**（妖精/丘陵从第 2 行起）。

### F.3 真机实测（r127，nxlink 日志）
朱伊安人一帧内的绘制顺序（x 全是 292）：
```
364 武器专精  格斗,投掷          cs=1876373
378 交涉      在交涉与商谈中更占上风  cs=1876593
393 朱伊安人不羁而自由。…朱伊       cs=1876593   ← 副本(说明第2行)
408 安人心灵手巧、…               cs=1876593
423 应性，…                      cs=1876593
439 集点，…                      cs=1876593
392 开锁      开锁的技巧            cs=1876593
406 演奏      令你能进行高水准的演奏   cs=1876593
421 朱伊安人不羁而自由。…朱伊       cs=1876593   ← 副本又一遍
436 / 451 / 467 …
```
- 副本每行都是**独立的单行 mes**（`t32hex` 显示 `brk=0`，即参数里没有 CR/LF）。
- 副本的 y 起点 = 上一个技能行的 y + 15（= 贴图高度），即**副本跟着技能绘制的 cy 走、没有 pos 重置**。
- 只有说明最长的 6 个种族（艾沃达纳人/妖精/丘陵人/朱伊安人/巫妖/魔像）会露到技能列。

### F.4 已排除 / 未决
- 已排除：换行函数丢字、贴图缓存满、字体度量 0、hgio_mes 静默返回、贴图哈希撞车、
  notesel/noteget 行表残留（`CStrNote::Select` 无状态）、hspda 的 xnotesel（0 次调用）。
- **未决**：为什么"技能列表绘制"会取到说明的尾部行。r128 探针已加入
  `t34 noteget`（记录 noteget 的 cs/行号/内容）与放宽到 x∈[240,340] 的 `t33hex`，
  预期一次复现即可看出是"行缓冲残留"还是"字符串变量复用"。

### F.5 关键日志/脚本
- `r127_nxlink.log`（t33hex+t33pc，确认副本 cs=1876593）
- `_sync/scan_t33.py`、`scan_t33order.py`（配对 cs 并还原绘制顺序）
- Eden 日志 `hsp3dish_boot.log` 的 `t25`（cs + 文本，本地运行中文未损坏）用于确认
  说明 cs=1875545、技能 cs=1876373/1876593。
- 提醒：真机 stdout 里非 ASCII 会被替换，探针必须打**十六进制**。

---

## 附录 G：种族面板重叠的定位与修复（2026-10-06，r129~r135 · 已结案）

> 结论先行：**这是 2.30 汉化版自身的脚本/数据问题，不是移植层缺陷**。
> 修复方式是移植层「只跳过画错的那一次 blit」，**不改游戏数据**。

### G.1 对照实验（判定问题归属）
同一支探针版 nro 不动，只换游戏数据，在 **PC（Windows）** 上跑同一界面：

| 版本 | 运行环境 | 种族面板右栏 | 截图 |
|---|---|---|---|
| **2.30 汉化版**（`elonaplus_free.exe`） | PC | **有重叠** | `_sync/pc_elona3.png` |
| **2.32 官方日文版**（`elonaplus.exe`） | PC | **无重叠** | `_sync/t232_pw2.png` |

→ 同一份汉化数据在 PC 上**同样重叠** ⇒ 与 Switch 移植层无关；
2.32 无此现象 ⇒ 也不是 OpenHSP 运行时的通病。

### G.2 证据链（Eden + 真机交叉验证）
- 副本的 `cs` 与「技能列表绘制」那段脚本**完全相同**（`cs=1876593`，见附录 F.1）
  ⇒ 是技能列表的绘制代码把说明的**尾部行**画了出来。
- 副本起点 = 上一个技能行的 `y + 15`（= 贴图高度），即**跟着技能绘制的 `cy` 走、无 pos 重置**。
- 只有说明最长的 **6 个种族**（艾沃达纳人/妖精/丘陵人/朱伊安人/巫妖/魔像）会露到技能列。
- Eden 本地日志 `hsp3dish_boot.log`（中文未损坏）与真机 `nxlink` 日志（hex）两路互证。

### G.3 途中修掉的一个真实缺陷：`sortnote`（r131/r132）
- **r93 的 stub 遗留问题**：`sortnote` 被 stub 后 `sortget` 会取到**陈旧的排序索引** → 返回错误的源行。
- **r131**：恢复真实实现（`NoteToData` 加写界 `max`，绝不越写 `DataIni()` 的容量）。
- **r132**：实测发现游戏会在 `userNpc_update` 里用**空 note** 调用它，原实现直接
  `throw HSPERR_ILLEGAL_FUNCTION` → 启动即 `err=3` 死亡（日志证据 `userNpc_update:1 ct:0`）。
  修复：`pv->flag != HSPVAR_FLAG_STR` 或行数 `<= 0` 时**直接跳过**（等价于旧 stub 的无害行为）。

### G.4 失败的隐藏方案（r133）→ 成功的方案（r134）

| 轮次 | 做法 | 结果 |
|---|---|---|
| r133 | 在 `hgio_mes` 里**提前 `return 0`** 隐藏错行 | ❌ **启动卡死**（连续两次复现）；破坏了后续绘制状态推进 |
| **r134** | **只跳过最后一次 `hgio_fontcopy`（blit）**，`sw_mes_clean`/居中/贴图注册等**前置步骤全部保留** | ✅ 画面干净；`t38 hide` 命中 27 行，**无误伤** |

**r134 判定规则**（正式版逻辑，`hgiox_switch.cpp` 的 `hgio_mes` 末段）：

```c
if ( bm->cx >= 286 && bm->cx <= 298 && bm->cy >= 385 && bm->cy <= 520 ) {
    /*  真技能行 = "<技能名><补白空格><说明>"，一定含连续两个半角空格；
        说明副本是普通句子（如"抗性。"），没有连续空格 → 判为副本  */
    sw_hide = !padded;
}
if ( !sw_hide ) { hgio_fontcopy( ... ); }   /* 只跳过 blit */
```

### G.5 收尾：探针清理（r135，CI 成功并已推真机）
- 移除：`t23`（keyobj/cls/redraw/gsel/gcopy/screen/setcls/present）、`t24`、`t25`、`t26`、
  `t31hex`、`t32hex`、`t32pc`、`t33hex`、`t33pc`、`t36`、`t38`。
- **保留全部功能修复**：r104（CR 副本跳过）、r131/r132（`sortnote`）、r134（错行隐藏），只删各自的日志。
- `dllshim_switch.cpp`、`switch_input.cpp` 的 `SWITCH_DIAG` **置 0** ⇒ 正式版不再向 SD 卡刷探针日志
  （nxlink stdout 因此几乎为空，**属预期**）。
- 涉及 7 个文件、**-287 行**：`hgiox_switch.cpp`、`hsp3gr_dish_switch.cpp`、`hspwnd_dish.cpp`、
  `hsp3int.cpp`、`glue_switch.cpp`、`dllshim_switch.cpp`、`switch_input.cpp`。
- **r136 续清**：`dllshim_switch.cpp` 中**不受 `SWITCH_DIAG` 控制**的无条件日志已移除（见 **G.8**）。

### G.6 提交与产物
| 轮次 | commit | 说明 |
|---|---|---|
| r131 | `51a0ac3` | restore sortnote (bounded) |
| r132 | `6ed2211` | sortnote skips empty/non-string notes |
| r133 | `d7f888a` → revert `ed8b6d8` | 提前 return 卡死，已回退 |
| r134 | `d1b6332` | hide the stray rows by skipping only the blit |
| r135 | `5e3ce01` | drop the diagnostic probes |
| r136 | `6dc1fa8` | remove the unconditional P3 logging from dllshim |
| r137 | `ce3aaa7` | drop the per-open file access trace (hsp3file write/ok) |

- 产物：`_sync/build_r135/hsp3dish.nro`（9408568 B，MD5 `8333d35d8aa7440244b564d8fc06612d`）。
- 真机日志：`r135_nxlink.log`（探针已清，文件为空属正常）。
- 补丁脚本：`_sync/patch_r131.py` ~ `patch_r135b.py`。

### G.8 续清 `dllshim_switch.cpp` 的无条件日志（r136）

r135 只清掉了"探针"，`dllshim_switch.cpp` 里还有一批**不受 `SWITCH_DIAG` 控制**的 `printf`，
正式版仍会把它们写到 stdout / SD 卡。

**移除**（成功路径 + 参数形状探针）：
- `### bitcheck ... -> set`（每次按键一行）
- `RemoveDirectory '<path>'`（仅成功；`FAIL` 行保留）
- `## xnotesel` / `## xnoteadd`（hspda no-op trace）
- `### sortval ...` / `### sortnote ...` + `hspda_probe()` / `hspda_probe_args()` 两个函数
- `zOpen '<path>' ... -> h=`（每次 gz 打开；`zOpen FAIL` 保留）
- `zRead` / `zWrite` / `zClose`（每次 gz 读/写/关）
- `### <lib>: argument N (sptr) absent -> NULL`

**有意保留**（量小且有诊断价值）：
- 启动 banner（现为 `hsp3switch: build r137`）
- `pruned stale save`、`RemoveDirectory FAIL`、`zOpen FAIL`
- **每次运行仅一次**的 `script end: err=...` + `z.hpi alloc/read/write/close/fail` 汇总
  （同时仍写入 SD 卡 `hsp3exit.log`，stdout 没了也能查）
- shim 告警（`N parameters, shim limit`、`unsupported parameter type`）
- 所有 `SWITCH_DIAG` 保护的诊断（宏已为 0）

**净变化**：`dllshim_switch.cpp` **-113 行**（3 insertions / 116 deletions）。
**仍会输出**：`hsp3file:` 逐次文件访问 trace（→ **r137 已清，见 G.9**）；`hsp3text:` 文本注册 trace 与 overlay 构建日志仍在。
**提交** `6dc1fa8`；产物 `_sync/build_r136/hsp3dish.nro`（9404472 B，MD5 `390bada0e4474de39c62c130cbd1ff3d`）。

### G.9 续清逐次文件访问 trace（r137）

`glue_switch.cpp` 的 `__wrap_fopen()` 每成功打开一个文件就打一行
（读 cap 150、写 cap 300），正式版每次载入/存档都会产生几十上百行。

**移除**：
- `hsp3file: write '<path>' (mode ...)`（每次写打开，cap 300）
- `hsp3file: ok   '<path>'`（每次读打开，cap 150）
- 随之无用的 `shown` / `shown_w` 预算变量

**保留**（有信号且量小）：
- `hsp3file: FAIL '<path>' (mode ...)`（mkdir-p 自愈后仍失败）
- `hsp3file: mkdir-p opened '<path>'`（自愈触发，每个新目录一次）

**仍会输出**（如需彻底安静请再说）：
- `hsp3text: #N <text>` / `hsp3text: (r) <text>` —— 文本注册 trace（新字符串一行，带 5 秒去重）
- `hsp3screen: <op> -> …` —— 屏幕操作失败告警（只打前几次）
- `hsp3switch: throw …` / `### Unsupported DLL call …` —— 真错误/告警，**建议保留**
- overlay 构建日志（`overlay built …`）

**提交** `ce3aaa7`；产物 `_sync/build_r137/hsp3dish.nro`
（9400376 B，MD5 `27f81043efe630f95c4747880ea9c3f4`）。

### G.7 仍待复验
1. 真机 **r137**：种族面板右栏、开场剧情（韦尔尼斯段）、建号全流程、世界地图。
2. `{1}` 场景「渲染帧全停」卡死是否仍在（r137 后复测）。
3. 中文名闪退（附录 D）是否复现。

## 附录 H：消息栏（冒险日志）文字重叠 · 真机独有（2026-10-06，r138~r159 · **未解决，已回退**）

### H.1 现象（用户描述，长期存在）
- 真机：消息栏"叠字"，有的字只显示一部分（"一个字有百分之60遮住了"）
- 刷新是"下面的字往上移动"；**第一行最容易重叠，重叠的内容是下一行的**；**最下面两行基本正常**
- Eden 模拟器：完全正常，一直无法复现

### H.2 决定性对照实验（本次，10-06）
把**真机正在跑的同一份 nro**（r156，MD5 `A1767FB8847A70C52F01628386627B17`）复制进 Eden SD 卡，
用同样的 config 跑（把真机 `msg_trans` 也改成 `4`，两边一致）：

| 项 | Eden | 真机 |
|---|---|---|
| 二进制 | r156 `A1767FB8` | r156 `A1767FB8` |
| config | `msg_trans=4`、`msgLine=4`、`800x600` | 相同 |
| 字体 | `ipaexg.ttf` MD5 `4093871A...` | 相同 |
| gbk.tbl / charset | `683AF598...` / `gbk` | 相同 |
| **消息栏自拷贝** | `t46 copy dst=136,517 size=664x45 src=136,532 bmsrc=800x600 self=1 gmode=0` | **逐字相同** |
| 小自拷贝序列 | `hgio: selfblit 0,30 24x18 -> 24,18 gmode=0 tex=0 ret=0 scratch=64x64 err=0x0` ×24 | **逐字相同** |
| **画面** | **正常** | **重叠（错位一行）** |

**代码相同、配置相同、资源相同、连发出的 GL 调用序列都相同，只有 GPU 不同。**

### H.3 结论
真机 Tegra X1 是 **TBDR（分块延迟渲染）**；Eden 把 GLES 调用转译到 PC 桌面 GPU（即时模式）。
消息栏刷新 = **同一张纹理内、两个矩形部分重叠的自拷贝**（整体上移一行 15px），
在 GL 规范里是 **undefined behavior**：即时模式给出正确像素，TBDR 可能读到尚未写回的行。

这也解释了"第一行最容易重叠、重叠的是下一行内容、下面两行正常"——
整块上移一行时，上面的行读到了旧内容。

### H.4 已排除（都有字节级证据）
- **字体**：真机与 Eden 的 `ipaexg.ttf` **MD5 完全相同**（`4093871A7F48E43B9CE7C38DA0C34809`，9745792 B），
  `gbk.tbl`（`683AF598737EDB878C9D90251BEED644`）、`charset.txt`（`gbk`）也相同；
  日志里两边的字体探测/加载逐字相同。
  引擎消息栏字体路径**硬编码**为 `sdmc:/switch/openhsp/ipaexg.ttf`（`hgiox_switch.cpp` 的 `TTF_FONTFILE`），
  无候选、无 fallback。`switch_overlay.cpp` 里那 3 个候选（`ipaexg.ttf` / `.simhei` / `.notovf`）**只服务左右黑边提示条**。
  真机 `switch/openhsp/` 下的 5 个字体条目（`ipaexg.ttf` / `.ipaex` / `.orig` / `.notovf` / `.simhei`）
  里有 4 个是历史备份，游戏不会读。
- **config**：真机 `msg_trans=0` vs Eden `4` 是唯一实质差异（另 `title_dialog` 1/0）。
  把真机改成 `4` 后**问题依旧** → 与配置无关。
- **代码逻辑**：r152 = r150 回退 = r137 的渲染行为；Eden 跑 r137 和 r156 都正常。

### H.5 失败的修复尝试（统统无效，均已回退）
| 版本 | 做法 | 结果 |
|---|---|---|
| r145~r147 | 在 `hgio_copy` 里按落点拦截/跳过消息栏区域 | 误伤（把图集**源坐标**当屏幕落点）→ 消息栏消失 / 小地图异常 |
| r150 | 去掉 `bm->type != HSPWND_TYPE_MAIN`，让主屏自拷贝走 scratch | 消息栏整块消失、日志停止更新 |
| r153~r155 | `glReadPixels` 从 `sw_main_fbo` 读回再画 | 已读到正确 FBO（`t52 readback fbo=1 main_ok=1 st=0`）但画面仍坏 |
| r157 | 在 direct 路径的重叠自拷贝前加 `glFinish()` | 真机无任何变化 |
| r158 | 恢复主屏走 scratch **并修好 r150 的 FBO 恢复 bug** | 真机仍重叠 |

**r150 失败的真正机制（本次定位）**：`hgio_copy` 的 scratch 路径末尾用
`sw_find(bm)` 恢复目标 FBO，而 `sw_find()` **只认识离屏目标，对主屏返回 NULL**，
于是 scratch FBO 一直绑着，拷贝把 scratch 画进了它自己 → 消息栏被擦掉。
r158 已把这个 bug 修好（改用 `sw_bind_target()`），但**真机依然重叠**——
说明"主屏纹理在 capture 时读不到实时内容"这个更早的猜测，或 scratch 链条上还有别的问题。

### H.6 关键代码位置
- `hgiox_switch.cpp` `hgio_copy()`（约 2930 起）：自拷贝判定 + scratch 路径
  - `sw_same` / `sw_ov`（部分重叠判定，约 3076~3090）
  - `( bm->type != HSPWND_TYPE_MAIN )`（主屏排除，r152 状态，约 3095）
- `sw_scratch_ensure()` / `sw_scratch_capture()`（约 799~884）：中转实现
- `sw_bind_target()`（1166）：**主屏唯一的正确绑定入口**（内部绑 `sw_main_fbo`）
- `sw_exp_fbo()`（383）：主屏 → `sw_main_fbo`
- `sw_tex_src()`（约 760~796）：主屏（`texid<0`）→ 包一个 `sw_main_tex` 的 TEXINF
- 既有先例：`sw_fcgraph_sub` 用 `glFinish()` 处理"后续 gcopy 采样刚写过的纹理"（2898~2902）；
  `sw_glFinish()` 在 `gl_finish == NULL` 时是 **no-op**（`gles1_shim.cpp` 943）

### H.7 下一步方向（接手直接可用）
1. **像素级 dump 对比**（最该先做）：在消息栏滚动前后 dump 主屏该区域（Eden vs 真机），
   确认错位到底是**读旧行**还是**没擦干净**。这一步从没做过，可能直接推翻"自拷贝是真凶"。
   现成工具：`sw_dump_fbo()`（890），r155 曾导出到 `sdmc:/dump/`。
2. **CPU 中转**：`glReadPixels` 读源矩形到内存 → `glTexSubImage2D` 上传到独立纹理 → 绘制。
   与 r153~r155 的区别：那几次是"读回后直接画"，没有经过独立纹理。
3. **双缓冲消息栏**：把消息栏渲染到独立离屏纹理，滚动在离屏上做，再整体合成到主屏。
4. 先确认 `glFinish` 在 Tegra 上**真的执行**（否则 r157 的实验不能算数）。

### H.8 当前状态（r159）
- 源码已回退到 **r152 可用状态**（`git checkout 54c6608 -- <两个文件>`，提交 `c511f71`）
- 真机 SD 卡 `switch/hsp3dish.nro` 已同步为 r159（= r152 行为）
- 消息栏重叠**未解决**，以上线索完整保留

## 附录 I：PCC 发色 24/28/29 发型丢失 · **已修复（r169）**

### I.1 现象
- 创建人物时，**发色选 24 / 28 / 29**，发型渲染异常：只剩几根黑色线条，或直接变成光头
- **Eden 和真机都有**（不是 GPU 差异），其他发色正常

### I.2 根因链（用 2.32 官方源码定位，非猜测）
数据来源：`6abe770cb851cd8e44ed2318\elona232src`（**2.32 脚本源码**）

1. **角色外观由 `graphic/PCCs/*.bmp` 拼出**。发型图恒为 `pcc_hair_2.bmp`
   （`chips.hsp`：文件名用 `pcc(PCC_HAIR) \ 1000` = **发型编号**，而发型固定为 2），
   颜色用 `pcc(PCC_HAIR) / 1000` = **发色编号** 查 `c_col` 表。
2. **发色表**（`init.hsp:22883`，`dim c_col, 3, 30`）：
   | 界面编号 | c_col 值 |
   |---|---|
   | 24 | `150,150,150` |
   | 28 | `180,180,180` |
   | **29** | **`230,230,230`** |
3. **`chips.hsp` 的 `create_pcpic` 对每个部件这样做**：
   ```hsp
   pos 128, 0
   picload ...pcc_hair_N.bmp, 1      ; 部件图背景是青色 RGB(43,133,133)
   color 0,0,0
   boxf 256,0,384,198                ; (256..384) 清黑
   gmode 4, , , 256                  ; 用"指定色"当透明色
   color 43,133,133                  ; ← 青色
   pget 128, 0
   pos 256, 0
   gcopy 10+arg1, 128,0,128,198      ; gmode 4：青背景被丢弃
   pos 256, 0
   gfini 128, 198
   gfdec2 <c_col 查出的发色 RGB>       ; 给部件减色
   gmode 2                           ; ← 沿用上一次设的 key
   color 0,0,0
   pos 0, 0
   gcopy 10+arg1, 256,0,128,198      ; 贴到角色图区
   ```
4. **部件图的真实像素**（实测 `pcc_hair_2.bmp`）：背景 = 青色 `(43,133,133)` 占 84%，
   头发 = **灰色 104~198**。
5. **`gfdec2 230,230,230` 用饱和减法**（`dst = max(0, dst-c)`，见 Hspext `fcgraph.cpp:98`）
   → **头发 104~198 全部被减到 0**。
6. 而 PCC 图区的**背景也是 (0,0,0)**，`gmode 2` 与最终贴图的 `gmode 2, 32, 48`
   **都把 0,0,0 当透明** → **被减黑的和被丢弃的是同一个值，头发被一起丢掉** → 光头/残线。
   `150/180/230` 三个高值都会让头发归零，所以正好是 24/28/29 三个色号。

**为什么 PC 版正常**：PC 版上这几个发色同样是黑色头发但**形状完整**，
说明它的减法**不会把灰色压成 0**（否则会和背景一样被丢弃）。

### I.3 修复（r169，一处改动）
把 `sw_fcgraph_tint()`（`hgiox_switch.cpp`）里 gfdec2 的混合方式从
**饱和减**改成**乘性减**：

```c
	if ( add == 0 ) {
		/*	r169: multiply, do not saturate.  dst * (255-c)/255 keeps 0
			at 0 (the transparent background and the key still work) but
			leaves a grey pixel small rather than clamping it to 0.		*/
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_COLOR );   /* dst * (1-src) */
	} else {
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
	}
```

效果：
- **黑底（0）**：`0 × k = 0` → 仍是 0 → 色键照常生效、背景照常消失 ✓
- **灰色头发（145）**：`145 × 25/255 ≈ 14` → 很暗但**非 0** → **不再被当透明**
  → 头发显示为**深黑但完整**，与 PC 版一致 ✓
- `gfinc`（加法）路径**不变**

### I.4 排查过程要点（含走过的弯路）
- 先怀疑**字体**、**config**（`msg_trans`）、**14 位色深**、**8bpp BMP** 等 —— 均用数据排除
- 关键转折：**拿到 2.32 源码**后，从 `chips.hsp` + `init.hsp` 直接读出完整机制
- 中途试过 **r163/r166「sticky colour key」**（与 PC 版的 `GetAttrOperation` 行为一致）：
  头发确实恢复了，但**其他部件全变黑**——因为色键换成青色后，
  (256,0) 区的黑底不再被丢弃。这条思路**方向对但解错了层**，正解是"别把头发减到 0"
- 探针教训：诊断探针的**条数上限**会被界面重复操作占满（120 条全被菜单消耗），
  **必须按目标缓冲过滤**（`bm->sx == 384`）才拿得到有效数据

### I.5 相关提交
| 版本 | 内容 |
|---|---|
| r169 | **修复**：gfdec2 改乘性减（提交 `3f77004`） |
| r166/r167 | sticky colour key 尝试 + PCC 专用 key 探针（已回退/保留探针） |
| r165/r168 | `gfdec2` 后像素读回探针（已定位根因） |
| r162 | 打印每次不同的 tint 值（发现 tint 序列与发色无关，从而转向源码） |

### I.6 验证
- **Eden**：发色 24/28/29 发型正常显示（用户确认"现在没问题了"）
- **真机**：r169 推送验证

## 附录 J：真机卡顿根因 & BGM 解决（r170 / r171）

### J.1 卡顿的真凶不是探针，是日志在写 SD 卡
- `main_dish.cpp` 的 `sw_log_open()` 里写着 `freopen( HSP3SWITCH_LOG, "w", stdout )`
  —— 把 **stdout 重定向到卡上的 `hsp3dish_boot.log`**；`sw_fbo_log()` / `sw_say()` 每行都 `fflush( stdout )`
- 从 hbmenu 启动时（没有 nxlink），**每一次绘制都在真写 SD 卡**。Eden 里那份日志已经涨到 **99 MB**
- Eden 的 stdout 是控制台，写完即弃 → 完全不卡。**这就是长期"Eden 正常、真机卡"的真正原因**
  （r170 删掉的 glReadPixels 只在 PCC 界面生效，影响小得多）
- **r171 修复**：只有 `<appdir>/log_on` 文件存在（或 nxlink 已连接）时才开卡上日志
  - 正常玩：无日志、无 SD 写
  - 要排查：在 `sdmc:/switch/openhsp/` 放一个空文件 `log_on` 即可恢复

### J.2 r170：清理发色排查遗留的探针（`hgiox_switch.cpp`）
| 版本 | 探针 | 说明 |
|---|---|---|
| r168 | 每次调用读 **8 次 `glReadPixels`** | Tegra 上是完整 GPU 同步，开销最大的一条 |
| r162 | 每个不同 tint 值写一行日志 | 游戏内列表高亮每次都在调 gfdec/gfinc |
| r166 | 每个不同色键写一行日志 | 同上 |

- `SWITCH_DIAG` 由 1 改回 0（该宏自己的注释就写着「0 = shipping build」）
- r169 的发色修复（乘性减 `GL_ZERO / GL_ONE_MINUS_SRC_COLOR`）**原样保留**

### J.3 config.txt 只读（r170）
- 拦截点：`glue_switch.cpp` 的 `__wrap_fopen`（链接器 `--wrap=fopen`，**所有文件打开的唯一入口**）
- 目标 basename 为 `config.txt` 的写入改道到 `sdmc:/switch/openhsp/tmp/config.ro`；读取完全不受影响
- 路径链路：Elona `notesave`（help.hsp:841/3670）→ `HSPEVENT_FWRITE` → `hsp3_binsave` → `fopen(...,"wb")`
- 效果：游戏内「环境设置」改画质/分辨率仍当次生效，但**重启后一律恢复卡上那份安全配置**，
  不会再因为被写坏的配置而黑屏/死机

### J.4 BGM 解决：SDL_mixer 的 TiMidity 音色库（原风险 R4）
- **根因**：Elona 的 88 首 BGM **全是 `.mid`**。`config.txt` 的 `music` 决定走哪条路
  （init.hsp:7286-7317）：
  - `1` → DirectMusic（`DMLOADFNAME`/`DMPLAY`）→ Switch 上 hmm.dll 是 no-op stub → **必然无声**
  - `2` → `mmload`/`mmplay` → SDL2_mixer（**Switch 上必须用这个**）
  - `0` → 关音乐
- `main_dish.cpp` **早已**写好 `setenv( "TIMIDITY_CFG", HSP3SWITCH_APPDIR "/timidity.cfg", 1 )`，
  缺的只是音色库文件 —— 这就是"把音色库放到游戏目录下就能播 MIDI"的那个办法
- **做法**：下载 SDL_mixer 官方 GUS patch 包
  `https://www.libsdl.org/projects/old/SDL_mixer/timidity/timidity.tar.gz`（14.8 MB），放置到 SD 卡：
  - `<appdir>/timidity.cfg` —— 把里面所有 `instruments/` 前缀改成**绝对路径**
    `sdmc:/switch/openhsp/timidity/instruments/`（相对路径依赖 cwd，绝对路径更稳）
  - `<appdir>/timidity/instruments/*.pat` —— 192 个文件，18 MB
- **前提**：`config.txt` 的 `music` 必须是 `"2"`
- **验证（Eden）**：日志由
  `[MMMan] Failed sdmc:/switch/openhsp\sound\theme.mid on bank #0 (1)`
  变为 `[MMMan] Loaded sdmc:/switch/openhsp\sound\theme.mid on bank #0`
- 本地成品目录：`_sync/timidity_out/`

### J.5 本轮交付与验证
| 项目 | 值 |
|---|---|
| 源码 HEAD | r171（提交 `2ba4dcc`） |
| 产物 | `hsp3dish.nro`，MD5 `B55D3838F4D2BEF8D922DE886A78841F`，9400376 B |
| 真机已推 | nro（校验 MD5 一致）、`timidity.cfg`、`timidity/instruments`（192）、`config.txt`（`music="2"`） |
| 待用户验证 | 卡顿是否消失、是否听到 BGM |

- **MTP 注意**：Windows Shell 会隐藏 `.cfg` / `.txt` 扩展名 —— 设备上的 `timidity.cfg` 与 `config.txt`
  在 Shell 里显示为 `timidity` 和 `config`。用 `Name` 精确匹配带扩展名的文件会找不到，
  必须按**去扩展名后**的名字匹配。
- **MTP 推送**：nro 必须先 `InvokeVerb("delete")` 删旧文件再 `CopyHere`，否则静默失败；
  文件夹（192 个 patch）用 `CopyHere($folderItem, 16)` 一次递归复制即可。
