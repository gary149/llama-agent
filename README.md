# llama.cpp

![llama](https://user-images.githubusercontent.com/1991296/230134379-7181e485-c521-4d23-a0d6-f7b3b61ba524.png)

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp)](https://github.com/ggml-org/llama.cpp/releases)
[![Server](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml/badge.svg)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)

[Manifesto](https://github.com/ggml-org/llama.cpp/discussions/205) | [ggml](https://github.com/ggml-org/ggml) | [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md)

**LLM inference in C/C++** — Run large language models locally with minimal setup and state-of-the-art performance.

---

## Quick Start

```sh
# Install via package manager
brew install llama.cpp    # macOS
nix-shell -p llama-cpp    # NixOS
winget install llama.cpp  # Windows

# Or download from releases / build from source
# See: docs/install.md • docs/build.md • docs/docker.md
```

```sh
# Run a model from Hugging Face
llama-cli -hf ggml-org/gemma-3-1b-it-GGUF

# Launch OpenAI-compatible API server
llama-server -hf ggml-org/gemma-3-1b-it-GGUF

# Use a local model file
llama-cli -m my_model.gguf
```

Need a model? Browse [GGUF models on Hugging Face](https://huggingface.co/models?library=gguf&sort=trending) or see [Obtaining Models](#obtaining-and-quantizing-models) below.

---

## Why llama.cpp?

- **Zero dependencies** — Plain C/C++ implementation
- **Broad hardware support** — Apple Silicon, NVIDIA/AMD/Intel GPUs, CPUs
- **Efficient quantization** — 1.5-bit to 8-bit integer quantization
- **Hybrid inference** — CPU+GPU for models larger than VRAM

<details>
<summary><strong>Full feature list</strong></summary>

- Apple silicon optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- Custom CUDA kernels for NVIDIA GPUs (AMD via HIP, Moore Threads via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference for models larger than VRAM capacity

</details>

---

<details>
<summary><strong>Recent API Changes & Hot Topics</strong></summary>

#### API Changelogs
- [Changelog for `libllama` API](https://github.com/ggml-org/llama.cpp/issues/9289)
- [Changelog for `llama-server` REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

#### Hot Topics
- **[Guide: Using the new WebUI](https://github.com/ggml-org/llama.cpp/discussions/16938)**
- [Guide: Running gpt-oss with llama.cpp](https://github.com/ggml-org/llama.cpp/discussions/15396)
- [Better packaging for downstream consumers](https://github.com/ggml-org/llama.cpp/discussions/15313)
- [gpt-oss with native MXFP4](https://github.com/ggml-org/llama.cpp/pull/15091) | [NVIDIA Collaboration](https://blogs.nvidia.com/blog/rtx-ai-garage-openai-oss)
- [Multimodal support in llama-server](https://github.com/ggml-org/llama.cpp/pull/12898) | [docs](./docs/multimodal.md)
- [VS Code extension for FIM](https://github.com/ggml-org/llama.vscode) | [Vim/Neovim plugin](https://github.com/ggml-org/llama.vim)
- [Hugging Face Inference Endpoints support](https://github.com/ggml-org/llama.cpp/discussions/9669)
- [GGUF editor](https://huggingface.co/spaces/CISCai/gguf-editor)

</details>

---

## Supported Backends

| Backend | Target Devices |
| --- | --- |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [CUDA](docs/build.md#cuda) | NVIDIA GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Vulkan](docs/build.md#vulkan) | Cross-platform GPU |
| [SYCL](docs/backend/SYCL.md) | Intel / NVIDIA GPU |

<details>
<summary><strong>All backends</strong></summary>

| Backend | Target Devices |
| --- | --- |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [SYCL](docs/backend/SYCL.md) | Intel and NVIDIA GPU |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [CUDA](docs/build.md#cuda) | NVIDIA GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [WebGPU [In Progress]](docs/build.md#webgpu) | All |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [Hexagon [In Progress]](docs/backend/hexagon/README.md) | Snapdragon |

</details>

---

## Obtaining and Quantizing Models

Download GGUF models directly from [Hugging Face](https://huggingface.co/models?library=gguf&sort=trending) or use the `-hf` flag:

```sh
llama-cli -hf ggml-org/gemma-3-1b-it-GGUF
```

<details>
<summary><strong>More options</strong></summary>

#### Alternative model sources
Set `MODEL_ENDPOINT` environment variable to download from other sources:
```sh
MODEL_ENDPOINT=https://www.modelscope.cn/ llama-cli -hf model-name
```

#### Converting models to GGUF
Models in other formats can be converted using the `convert_*.py` scripts in this repo. The model must be in [GGUF format](https://github.com/ggml-org/ggml/blob/master/docs/gguf.md).

#### Online tools
- [GGUF-my-repo](https://huggingface.co/spaces/ggml-org/gguf-my-repo) — Convert and quantize models
- [GGUF-my-LoRA](https://huggingface.co/spaces/ggml-org/gguf-my-lora) — Convert LoRA adapters ([more info](https://github.com/ggml-org/llama.cpp/discussions/10123))
- [GGUF-editor](https://huggingface.co/spaces/CISCai/gguf-editor) — Edit GGUF metadata in browser
- [Inference Endpoints](https://ui.endpoints.huggingface.co/) — Host llama.cpp in the cloud

To learn more about quantization, see [quantize documentation](tools/quantize/README.md).

</details>

---

## CLI Tools

### [`llama-cli`](tools/cli) — Interactive Chat & Text Generation

```bash
llama-cli -m model.gguf
# Starts interactive conversation mode
```

<details>
<summary><strong>More examples</strong></summary>

**Custom chat template:**
```bash
llama-cli -m model.gguf -cnv --chat-template chatml
llama-cli -m model.gguf -cnv --in-prefix 'User: ' --reverse-prompt 'User:'
```

**Constrained output with grammar:**
```bash
llama-cli -m model.gguf -n 256 --grammar-file grammars/json.gbnf -p 'Request: schedule a call at 8pm; Command:'
# {"appointmentTime": "8pm", "appointmentDetails": "schedule a call"}
```

See [grammars/](grammars/) for samples and the [GBNF Guide](grammars/README.md) for writing custom grammars.

</details>

---

### [`llama-server`](tools/server) — OpenAI-Compatible API Server

```bash
llama-server -m model.gguf --port 8080
# Web UI: http://localhost:8080
# API: http://localhost:8080/v1/chat/completions
```

<details>
<summary><strong>More examples</strong></summary>

**Multi-user with parallel decoding:**
```bash
llama-server -m model.gguf -c 16384 -np 4  # 4 concurrent requests
```

**Speculative decoding:**
```bash
llama-server -m model.gguf -md draft.gguf
```

**Embedding server:**
```bash
llama-server -m model.gguf --embedding --pooling cls -ub 8192
```

**Reranking server:**
```bash
llama-server -m model.gguf --reranking
```

</details>

---

### [`llama-bench`](tools/llama-bench) — Performance Benchmarking

```bash
llama-bench -m model.gguf
```

<details>
<summary><strong>Sample output</strong></summary>

```
| model           |       size |     params | backend    | threads |    test |              t/s |
| --------------- | ---------: | ---------: | ---------- | ------: | ------: | ---------------: |
| qwen2 1.5B Q4_0 | 885.97 MiB |     1.54 B | Metal,BLAS |      16 |   pp512 | 5765.41 ± 20.55  |
| qwen2 1.5B Q4_0 | 885.97 MiB |     1.54 B | Metal,BLAS |      16 |   tg128 |   197.71 ± 0.81  |
```

</details>

---

### [`llama-perplexity`](tools/perplexity) — Model Quality Metrics

```bash
llama-perplexity -m model.gguf -f file.txt
# Final estimate: PPL = 5.4007 +/- 0.67339
```

Learn more about [perplexity](https://huggingface.co/docs/transformers/perplexity).

---

## Ecosystem

Explore the growing llama.cpp ecosystem: supported models, language bindings, applications, and deployment tools.

<details>
<summary><strong>Supported Models (100+)</strong></summary>

Typically finetunes of the base models below are supported as well.

Instructions for adding support for new models: [HOWTO-add-model.md](docs/development/HOWTO-add-model.md)

#### Text-only

- [X] LLaMA 🦙
- [x] LLaMA 2 🦙🦙
- [x] LLaMA 3 🦙🦙🦙
- [X] [Mistral 7B](https://huggingface.co/mistralai/Mistral-7B-v0.1)
- [x] [Mixtral MoE](https://huggingface.co/models?search=mistral-ai/Mixtral)
- [x] [DBRX](https://huggingface.co/databricks/dbrx-instruct)
- [x] [Jamba](https://huggingface.co/ai21labs)
- [X] [Falcon](https://huggingface.co/models?search=tiiuae/falcon)
- [X] [Chinese LLaMA / Alpaca](https://github.com/ymcui/Chinese-LLaMA-Alpaca) and [Chinese LLaMA-2 / Alpaca-2](https://github.com/ymcui/Chinese-LLaMA-Alpaca-2)
- [X] [Vigogne (French)](https://github.com/bofenghuang/vigogne)
- [X] [BERT](https://github.com/ggml-org/llama.cpp/pull/5423)
- [X] [Koala](https://bair.berkeley.edu/blog/2023/04/03/koala/)
- [X] [Baichuan 1 & 2](https://huggingface.co/models?search=baichuan-inc/Baichuan) + [derivations](https://huggingface.co/hiyouga/baichuan-7b-sft)
- [X] [Aquila 1 & 2](https://huggingface.co/models?search=BAAI/Aquila)
- [X] [Starcoder models](https://github.com/ggml-org/llama.cpp/pull/3187)
- [X] [Refact](https://huggingface.co/smallcloudai/Refact-1_6B-fim)
- [X] [MPT](https://github.com/ggml-org/llama.cpp/pull/3417)
- [X] [Bloom](https://github.com/ggml-org/llama.cpp/pull/3553)
- [x] [Yi models](https://huggingface.co/models?search=01-ai/Yi)
- [X] [StableLM models](https://huggingface.co/stabilityai)
- [x] [Deepseek models](https://huggingface.co/models?search=deepseek-ai/deepseek)
- [x] [Qwen models](https://huggingface.co/models?search=Qwen/Qwen)
- [x] [PLaMo-13B](https://github.com/ggml-org/llama.cpp/pull/3557)
- [x] [Phi models](https://huggingface.co/models?search=microsoft/phi)
- [x] [PhiMoE](https://github.com/ggml-org/llama.cpp/pull/11003)
- [x] [GPT-2](https://huggingface.co/gpt2)
- [x] [Orion 14B](https://github.com/ggml-org/llama.cpp/pull/5118)
- [x] [InternLM2](https://huggingface.co/models?search=internlm2)
- [x] [CodeShell](https://github.com/WisdomShell/codeshell)
- [x] [Gemma](https://ai.google.dev/gemma)
- [x] [Mamba](https://github.com/state-spaces/mamba)
- [x] [Grok-1](https://huggingface.co/keyfan/grok-1-hf)
- [x] [Xverse](https://huggingface.co/models?search=xverse)
- [x] [Command-R models](https://huggingface.co/models?search=CohereForAI/c4ai-command-r)
- [x] [SEA-LION](https://huggingface.co/models?search=sea-lion)
- [x] [GritLM-7B](https://huggingface.co/GritLM/GritLM-7B) + [GritLM-8x7B](https://huggingface.co/GritLM/GritLM-8x7B)
- [x] [OLMo](https://allenai.org/olmo)
- [x] [OLMo 2](https://allenai.org/olmo)
- [x] [OLMoE](https://huggingface.co/allenai/OLMoE-1B-7B-0924)
- [x] [Granite models](https://huggingface.co/collections/ibm-granite/granite-code-models-6624c5cec322e4c148c8b330)
- [x] [GPT-NeoX](https://github.com/EleutherAI/gpt-neox) + [Pythia](https://github.com/EleutherAI/pythia)
- [x] [Snowflake-Arctic MoE](https://huggingface.co/collections/Snowflake/arctic-66290090abe542894a5ac520)
- [x] [Smaug](https://huggingface.co/models?search=Smaug)
- [x] [Poro 34B](https://huggingface.co/LumiOpen/Poro-34B)
- [x] [Bitnet b1.58 models](https://huggingface.co/1bitLLM)
- [x] [Flan T5](https://huggingface.co/models?search=flan-t5)
- [x] [Open Elm models](https://huggingface.co/collections/apple/openelm-instruct-models-6619ad295d7ae9f868b759ca)
- [x] [ChatGLM3-6b](https://huggingface.co/THUDM/chatglm3-6b) + [ChatGLM4-9b](https://huggingface.co/THUDM/glm-4-9b) + [GLMEdge-1.5b](https://huggingface.co/THUDM/glm-edge-1.5b-chat) + [GLMEdge-4b](https://huggingface.co/THUDM/glm-edge-4b-chat)
- [x] [GLM-4-0414](https://huggingface.co/collections/THUDM/glm-4-0414-67f3cbcb34dd9d252707cb2e)
- [x] [SmolLM](https://huggingface.co/collections/HuggingFaceTB/smollm-6695016cad7167254ce15966)
- [x] [EXAONE-3.0-7.8B-Instruct](https://huggingface.co/LGAI-EXAONE/EXAONE-3.0-7.8B-Instruct)
- [x] [FalconMamba Models](https://huggingface.co/collections/tiiuae/falconmamba-7b-66b9a580324dd1598b0f6d4a)
- [x] [Jais](https://huggingface.co/inceptionai/jais-13b-chat)
- [x] [Bielik-11B-v2.3](https://huggingface.co/collections/speakleash/bielik-11b-v23-66ee813238d9b526a072408a)
- [x] [RWKV-6](https://github.com/BlinkDL/RWKV-LM)
- [x] [QRWKV-6](https://huggingface.co/recursal/QRWKV6-32B-Instruct-Preview-v0.1)
- [x] [GigaChat-20B-A3B](https://huggingface.co/ai-sage/GigaChat-20B-A3B-instruct)
- [X] [Trillion-7B-preview](https://huggingface.co/trillionlabs/Trillion-7B-preview)
- [x] [Ling models](https://huggingface.co/collections/inclusionAI/ling-67c51c85b34a7ea0aba94c32)
- [x] [LFM2 models](https://huggingface.co/collections/LiquidAI/lfm2-686d721927015b2ad73eaa38)
- [x] [Hunyuan models](https://huggingface.co/collections/tencent/hunyuan-dense-model-6890632cda26b19119c9c5e7)
- [x] [BailingMoeV2 (Ring/Ling 2.0) models](https://huggingface.co/collections/inclusionAI/ling-v2-68bf1dd2fc34c306c1fa6f86)

#### Multimodal

- [x] [LLaVA 1.5 models](https://huggingface.co/collections/liuhaotian/llava-15-653aac15d994e992e2677a7e), [LLaVA 1.6 models](https://huggingface.co/collections/liuhaotian/llava-16-65b9e40155f60fd046a5ccf2)
- [x] [BakLLaVA](https://huggingface.co/models?search=SkunkworksAI/Bakllava)
- [x] [Obsidian](https://huggingface.co/NousResearch/Obsidian-3B-V0.5)
- [x] [ShareGPT4V](https://huggingface.co/models?search=Lin-Chen/ShareGPT4V)
- [x] [MobileVLM 1.7B/3B models](https://huggingface.co/models?search=mobileVLM)
- [x] [Yi-VL](https://huggingface.co/models?search=Yi-VL)
- [x] [Mini CPM](https://huggingface.co/models?search=MiniCPM)
- [x] [Moondream](https://huggingface.co/vikhyatk/moondream2)
- [x] [Bunny](https://github.com/BAAI-DCAI/Bunny)
- [x] [GLM-EDGE](https://huggingface.co/models?search=glm-edge)
- [x] [Qwen2-VL](https://huggingface.co/collections/Qwen/qwen2-vl-66cee7455501d7126940800d)
- [x] [LFM2-VL](https://huggingface.co/collections/LiquidAI/lfm2-vl-68963bbc84a610f7638d5ffa)

</details>

<details>
<summary><strong>Language Bindings</strong></summary>

- Python: [ddh0/easy-llama](https://github.com/ddh0/easy-llama)
- Python: [abetlen/llama-cpp-python](https://github.com/abetlen/llama-cpp-python)
- Go: [go-skynet/go-llama.cpp](https://github.com/go-skynet/go-llama.cpp)
- Node.js: [withcatai/node-llama-cpp](https://github.com/withcatai/node-llama-cpp)
- JS/TS (llama.cpp server client): [lgrammel/modelfusion](https://modelfusion.dev/integration/model-provider/llamacpp)
- JS/TS (Programmable Prompt Engine CLI): [offline-ai/cli](https://github.com/offline-ai/cli)
- JavaScript/Wasm (works in browser): [tangledgroup/llama-cpp-wasm](https://github.com/tangledgroup/llama-cpp-wasm)
- Typescript/Wasm (nicer API, available on npm): [ngxson/wllama](https://github.com/ngxson/wllama)
- Ruby: [yoshoku/llama_cpp.rb](https://github.com/yoshoku/llama_cpp.rb)
- Rust (more features): [edgenai/llama_cpp-rs](https://github.com/edgenai/llama_cpp-rs)
- Rust (nicer API): [mdrokz/rust-llama.cpp](https://github.com/mdrokz/rust-llama.cpp)
- Rust (more direct bindings): [utilityai/llama-cpp-rs](https://github.com/utilityai/llama-cpp-rs)
- Rust (automated build from crates.io): [ShelbyJenkins/llm_client](https://github.com/ShelbyJenkins/llm_client)
- C#/.NET: [SciSharp/LLamaSharp](https://github.com/SciSharp/LLamaSharp)
- C#/VB.NET (more features - community license): [LM-Kit.NET](https://docs.lm-kit.com/lm-kit-net/index.html)
- Scala 3: [donderom/llm4s](https://github.com/donderom/llm4s)
- Clojure: [phronmophobic/llama.clj](https://github.com/phronmophobic/llama.clj)
- React Native: [mybigday/llama.rn](https://github.com/mybigday/llama.rn)
- Java: [kherud/java-llama.cpp](https://github.com/kherud/java-llama.cpp)
- Java: [QuasarByte/llama-cpp-jna](https://github.com/QuasarByte/llama-cpp-jna)
- Zig: [deins/llama.cpp.zig](https://github.com/Deins/llama.cpp.zig)
- Flutter/Dart: [netdur/llama_cpp_dart](https://github.com/netdur/llama_cpp_dart)
- Flutter: [xuegao-tzx/Fllama](https://github.com/xuegao-tzx/Fllama)
- PHP (API bindings and features built on top of llama.cpp): [distantmagic/resonance](https://github.com/distantmagic/resonance) [(more info)](https://github.com/ggml-org/llama.cpp/pull/6326)
- Guile Scheme: [guile_llama_cpp](https://savannah.nongnu.org/projects/guile-llama-cpp)
- Swift [srgtuszy/llama-cpp-swift](https://github.com/srgtuszy/llama-cpp-swift)
- Swift [ShenghaiWang/SwiftLlama](https://github.com/ShenghaiWang/SwiftLlama)
- Delphi [Embarcadero/llama-cpp-delphi](https://github.com/Embarcadero/llama-cpp-delphi)
- Go (no CGo needed): [hybridgroup/yzma](https://github.com/hybridgroup/yzma)
- Android: [llama.android](/examples/llama.android)

</details>

<details>
<summary><strong>UIs & Applications</strong></summary>

*(to have a project listed here, it should clearly state that it depends on `llama.cpp`)*

- [AI Sublime Text plugin](https://github.com/yaroslavyaroslav/OpenAI-sublime-text) (MIT)
- [cztomsik/ava](https://github.com/cztomsik/ava) (MIT)
- [Dot](https://github.com/alexpinel/Dot) (GPL)
- [eva](https://github.com/ylsdamxssjxxdd/eva) (MIT)
- [iohub/collama](https://github.com/iohub/coLLaMA) (Apache-2.0)
- [janhq/jan](https://github.com/janhq/jan) (AGPL)
- [johnbean393/Sidekick](https://github.com/johnbean393/Sidekick) (MIT)
- [KanTV](https://github.com/zhouwg/kantv?tab=readme-ov-file) (Apache-2.0)
- [KodiBot](https://github.com/firatkiral/kodibot) (GPL)
- [llama.vim](https://github.com/ggml-org/llama.vim) (MIT)
- [LARS](https://github.com/abgulati/LARS) (AGPL)
- [Llama Assistant](https://github.com/vietanhdev/llama-assistant) (GPL)
- [LLMFarm](https://github.com/guinmoon/LLMFarm?tab=readme-ov-file) (MIT)
- [LLMUnity](https://github.com/undreamai/LLMUnity) (MIT)
- [LMStudio](https://lmstudio.ai/) (proprietary)
- [LocalAI](https://github.com/mudler/LocalAI) (MIT)
- [LostRuins/koboldcpp](https://github.com/LostRuins/koboldcpp) (AGPL)
- [MindMac](https://mindmac.app) (proprietary)
- [MindWorkAI/AI-Studio](https://github.com/MindWorkAI/AI-Studio) (FSL-1.1-MIT)
- [Mobile-Artificial-Intelligence/maid](https://github.com/Mobile-Artificial-Intelligence/maid) (MIT)
- [Mozilla-Ocho/llamafile](https://github.com/Mozilla-Ocho/llamafile) (Apache-2.0)
- [nat/openplayground](https://github.com/nat/openplayground) (MIT)
- [nomic-ai/gpt4all](https://github.com/nomic-ai/gpt4all) (MIT)
- [ollama/ollama](https://github.com/ollama/ollama) (MIT)
- [oobabooga/text-generation-webui](https://github.com/oobabooga/text-generation-webui) (AGPL)
- [PocketPal AI](https://github.com/a-ghorbani/pocketpal-ai) (MIT)
- [psugihara/FreeChat](https://github.com/psugihara/FreeChat) (MIT)
- [ptsochantaris/emeltal](https://github.com/ptsochantaris/emeltal) (MIT)
- [pythops/tenere](https://github.com/pythops/tenere) (AGPL)
- [ramalama](https://github.com/containers/ramalama) (MIT)
- [semperai/amica](https://github.com/semperai/amica) (MIT)
- [withcatai/catai](https://github.com/withcatai/catai) (MIT)
- [Autopen](https://github.com/blackhole89/autopen) (GPL)

</details>

<details>
<summary><strong>Tools & Utilities</strong></summary>

- [akx/ggify](https://github.com/akx/ggify) – download PyTorch models from HuggingFace Hub and convert them to GGML
- [akx/ollama-dl](https://github.com/akx/ollama-dl) – download models from the Ollama library to be used directly with llama.cpp
- [crashr/gppm](https://github.com/crashr/gppm) – launch llama.cpp instances utilizing NVIDIA Tesla P40 or P100 GPUs with reduced idle power consumption
- [gpustack/gguf-parser](https://github.com/gpustack/gguf-parser-go/tree/main/cmd/gguf-parser) - review/check the GGUF file and estimate the memory usage
- [Styled Lines](https://marketplace.unity.com/packages/tools/generative-ai/styled-lines-llama-cpp-model-292902) (proprietary licensed, async wrapper of inference part for game development in Unity3d with pre-built Mobile and Web platform wrappers and a model example)
- [unslothai/unsloth](https://github.com/unslothai/unsloth) – 🦥 exports/saves fine-tuned and trained models to GGUF (Apache-2.0)

</details>

<details>
<summary><strong>Infrastructure & Deployment</strong></summary>

- [Paddler](https://github.com/intentee/paddler) - Open-source LLMOps platform for hosting and scaling AI in your own infrastructure
- [GPUStack](https://github.com/gpustack/gpustack) - Manage GPU clusters for running LLMs
- [llama_cpp_canister](https://github.com/onicai/llama_cpp_canister) - llama.cpp as a smart contract on the Internet Computer, using WebAssembly
- [llama-swap](https://github.com/mostlygeek/llama-swap) - transparent proxy that adds automatic model switching with llama-server
- [Kalavai](https://github.com/kalavai-net/kalavai-client) - Crowdsource end to end LLM deployment at any scale
- [llmaz](https://github.com/InftyAI/llmaz) - ☸️ Easy, advanced inference platform for large language models on Kubernetes.

</details>

<details>
<summary><strong>Games</strong></summary>

- [Lucy's Labyrinth](https://github.com/MorganRO8/Lucys_Labyrinth) - A simple maze game where agents controlled by an AI model will try to trick you.

</details>

---

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- See [good first issues](https://github.com/ggml-org/llama.cpp/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22) for tasks suitable for first contributions
- Read [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines
- Background: [Manifesto](https://github.com/ggml-org/llama.cpp/discussions/205) | [Changelog podcast](https://changelog.com/podcast/532)

---

## Documentation

| Topic | Link |
| --- | --- |
| CLI usage | [tools/cli/README.md](tools/cli/README.md) |
| Server API | [tools/server/README.md](tools/server/README.md) |
| Building | [docs/build.md](docs/build.md) |
| Docker | [docs/docker.md](docs/docker.md) |
| Android | [docs/android.md](docs/android.md) |
| GBNF grammars | [grammars/README.md](grammars/README.md) |
| Performance tips | [docs/development/token_generation_performance_tips.md](docs/development/token_generation_performance_tips.md) |
| GGML tips | [GGML Tips & Tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks) |

<details>
<summary><strong>Background & Research Papers</strong></summary>

If your issue is with model generation quality, please review these resources to understand LLaMA model limitations:

**LLaMA:**
- [Introducing LLaMA: A foundational, 65-billion-parameter large language model](https://ai.facebook.com/blog/large-language-model-llama-meta-ai/)
- [LLaMA: Open and Efficient Foundation Language Models](https://arxiv.org/abs/2302.13971)

**GPT-3:**
- [Language Models are Few-Shot Learners](https://arxiv.org/abs/2005.14165)

**GPT-3.5 / InstructGPT / ChatGPT:**
- [Aligning language models to follow instructions](https://openai.com/research/instruction-following)
- [Training language models to follow instructions with human feedback](https://arxiv.org/abs/2203.02155)

</details>

---

<details>
<summary><strong>Platform-Specific: XCFramework (iOS/macOS/visionOS)</strong></summary>

The XCFramework is a precompiled version of the library for iOS, visionOS, tvOS, and macOS. It can be used in Swift projects without compiling from source:

```swift
// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "MyLlamaPackage",
    targets: [
        .executableTarget(
            name: "MyLlamaPackage",
            dependencies: ["LlamaFramework"]),
        .binaryTarget(
            name: "LlamaFramework",
            url: "https://github.com/ggml-org/llama.cpp/releases/download/b5046/llama-b5046-xcframework.zip",
            checksum: "c19be78b5f00d8d29a25da41042cb7afa094cbf6280a225abe614b03b20029ab")
    ]
)
```

Modify the URL and checksum to use different versions.

</details>

<details>
<summary><strong>Shell Completions</strong></summary>

**Bash:**
```bash
build/bin/llama-cli --completion-bash > ~/.llama-completion.bash
source ~/.llama-completion.bash

# Add to .bashrc for persistence
echo "source ~/.llama-completion.bash" >> ~/.bashrc
```

</details>

<details>
<summary><strong>Dependencies</strong></summary>

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [stb-image](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [minja](https://github.com/google/minja) - Minimal Jinja parser in C++, used by various tools/examples - MIT License
- [curl](https://curl.se/) - Client-side URL transfer library, used by various tools/examples - [CURL License](https://curl.se/docs/copyright.html)
- [miniaudio.h](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain

</details>
