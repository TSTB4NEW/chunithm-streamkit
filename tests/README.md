# tests

不加载游戏的测试工具。

| 文件 | 用途 | 依赖 |
|---|---|---|
| `parse_test.cpp` | `ParseDanmaku` 单元测试（对象头 / 标准 / cmd 在后 / 带后缀） | 无 |
| `bili_auth_test.cpp` | 登录态、Cookie 头、Set-Cookie 解析、扫码接口、nav 校验 | 网络 |
| `bili_ws_test.cpp` | 直接跑弹幕客户端：房间解析、认证、收弹幕 | 网络 + `auth.txt` + Brotli |
| `verify_bili_ws.py` | Python 独立协议探针（大端包、WBI 签名、原始回包） | Python 3 |
| `host/main.c` | 模拟第三方进程加载 ASI 的宿主 | — |

## 一键运行

```bat
set ZIG=C:\path\to\zig.exe
tests\run_tests.cmd
```

## 真实弹幕测试

先构建（生成 `build_zig\br_*.obj`）：

```bat
build_zig.cmd
```

再编译并运行：

```bat
set ZIG=...
set CFLAGS=-target x86-windows-gnu -O2 -std=c++17 -Isrc -Ithird_party\brotli\include -D_WIN32_WINNT=0x0601 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
"%ZIG%" c++ %CFLAGS% -o tests\bin\bili_ws_test.exe tests\bili_ws_test.cpp build_zig\br_*.obj
tests\bin\bili_ws_test.exe "auth.txt" 123456 60
```

`auth.txt` 在 `<游戏>\bin\chunithm_streamkit\auth.txt`，由扫码登录生成，属账号凭据，勿提交。

## Python 探针

```bat
python tests\verify_bili_ws.py "auth.txt" 123456
```
