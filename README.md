# Qwen3-TTS 本機語音（C++ 版）

不需要 Python。引擎是 [qwentts.cpp](https://github.com/ServeurpersoCom/qwentts.cpp)（C++ / GGML / CUDA），介面是網頁（HTML/CSS/JS）。

## 使用

1. 第一次：雙擊 `build.bat`
   - 檢查 git、Visual Studio C++ Build Tools、CUDA Toolkit，缺的話可用 winget 自動安裝
   - 下載 qwentts.cpp 原始碼（固定在測試過的版本）並編譯出 `bin\t2v-server.exe`
   - 下載模型到 `models\`（VoiceDesign + Base + tokenizer，Q8_0 約 4.8 GB）
2. 之後：雙擊 `run.bat`，瀏覽器會自動開啟 http://127.0.0.1:7860

## 功能

- **設計聲音**：用文字描述聲音生成語音，可邊生成邊播放、固定 seed 重現
- **固定聲音 / 複製聲音**：把設計好的聲音、上傳的音檔或麥克風錄音存成聲音，之後重複使用
  - 有逐字稿：聲紋 + 逐字稿（ICL），最像
  - 沒逐字稿：只用聲紋

## 資料夾

| 路徑 | 內容 |
|---|---|
| `outputs\` | 每次生成的 WAV |
| `voices\` | 已存聲音：`.spk` `.rvq` `.txt` `.wav`（可直接給 qwen-tts CLI 的 `--ref-spk/--ref-rvq/--ref-text` 用） |
| `server\` | 伺服器原始碼 `t2v-server.cpp`、CMake 掛載檔 `t2v.cmake` |
| `web\` | 網頁介面 |

## 進階設定（在 cmd 先 `set` 再執行 bat）

- `set QUANT=BF16`：用 BF16 模型（品質最接近原版，兩個模型約 8 GB VRAM）；`Q4_K_M` 最省記憶體
- `set CUDA_ARCH=89`：指定 GPU 架構（預設 native 自動偵測，4070 是 89）
- `set PORT=8000`：改連接埠（run.bat）
- `set QWENTTS_REF=master`：改用 qwentts.cpp 最新版（未測試）

## 從原始碼建置的需求

- Windows 10/11 x64、NVIDIA 顯示卡
- Git、Visual Studio 2022 Build Tools（C++ 桌面開發，含 CMake / Ninja）、CUDA Toolkit 12.x 以上
- `build.bat` 產生的 `qwentts.cpp\`、`bin\`、`models\` 與執行資料 `outputs\`、`voices\` 都不進版控（見 `.gitignore`）

## 第三方元件與授權

- [qwentts.cpp](https://github.com/ServeurpersoCom/qwentts.cpp)：MIT，建置時下載，不包含在本 repo
- [Qwen3-TTS](https://github.com/QwenLM/Qwen3-TTS) 模型與 12Hz tokenizer：Apache 2.0
- GGUF 權重：[Serveurperso/Qwen3-TTS-GGUF](https://huggingface.co/Serveurperso/Qwen3-TTS-GGUF)
