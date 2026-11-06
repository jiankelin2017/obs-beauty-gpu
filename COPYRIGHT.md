# 版权与许可声明 / Copyright and License Notice

```
OBS Beauty GPU（OBS 美颜 GPU 版）
Copyright (C) 2026 Jian
```

本程序是自由软件：你可以依据自由软件基金会发布的 **GNU 通用公共许可证第 2 版**
的条款重新分发它和/或修改它。**不含**「或任何更新版本」选项（即 GPL-2.0 only）。

本程序按「希望它有用」的原则分发，但**不提供任何担保**，甚至不提供适销性或
特定用途适用性的默示担保。详情见 GNU 通用公共许可证。

你应当已随本程序收到一份 GNU 通用公共许可证的副本；如果没有，请写信给
Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
MA 02110-1301 USA。

**许可证全文见同目录的 [`LICENSE`](LICENSE)**（FSF 官方原文，未作任何改动）。

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; **version 2 of the License** only.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

The full text is in [`LICENSE`](LICENSE), taken verbatim from the Free Software
Foundation.

---

## 适用范围 / Scope

本许可覆盖本仓库中的原创部分：

- `01_源码/` 下的源代码
- 着色器（`01_源码/shaders/`）
- `07_安装包/` 下的安装程序源码
- 验证脚本与文档

**不覆盖**随本程序捆绑或加载的第三方组件：ONNX Runtime、YuNet 人脸检测模型，
以及可选的 BiSeNet / MediaPipe / 手部模型。这些组件仍适用各自的许可，条款见
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)。它们与 GPL 兼容，因此整个
安装包可以按 GPL-2.0 一并分发，但**必须保留其版权声明**。

本插件以动态链接方式使用 OBS Studio 的 `libobs`（同样为 GPL-2.0），与宿主采用
同一许可，因此对外分发时在许可层面不存在冲突。

This license covers the original work in this repository: the source code under
`01_源码/`, the shaders, the installer sources under `07_安装包/`, the
verification scripts, and the documentation.

It does **not** cover third-party components bundled with or loaded by this
program (ONNX Runtime, the YuNet face detection model, and the optional BiSeNet /
MediaPipe / hand models). Those remain under their own licenses — see
`THIRD-PARTY-NOTICES.md`. They are compatible with the GPL, so redistributing the
whole package is fine, but their copyright notices must be preserved.

---

## 为什么 LICENSE 里没有上面这些说明

GitHub、SPDX 工具与多数合规扫描器判断项目许可的方式，是把 `LICENSE` 与官方
许可原文做**全文比对**。只要文件里混入自定义段落（哪怕是版权行），匹配就会失败，
项目会被归为「Other」，无法按 GPL-2.0 检索，仓库页也不会显示许可徽章。

因此本仓库把两者拆开：

| 文件 | 内容 | 是否可改 |
|---|---|---|
| `LICENSE` | GPL-2.0 全文，**与 FSF 原文逐字节一致**（17,984 B，SHA256 `edaef632…b233f6`） | **不可改**（GPL 正文本身要求原样分发） |
| `COPYRIGHT.md` | 版权行、版本选择、适用范围、第三方边界 | 可改 |

## 想让下游能升级到 GPL-3.0 吗

把 `COPYRIGHT.md` 上方的「version 2 of the License」改成标准写法即可：

```
... either version 2 of the License, or (at your option) any later version.
```

`LICENSE` 文件本身无需改动 —— 它只是第 2 版的文本，`or later` 是授权声明里的
措辞，不是许可文本的一部分。
