# AGENTS.md — AI 协作约定

本仓库欢迎 AI 参与开发，但所有改动必须走 **Pull Request**，不直接推 main。

## 规则

1. **分支**：基于最新 `main` 开分支，命名 `ai/<主题>`（例如 `ai/fix-gdt`、`ai/feat-ipc`）。
2. **提交**：小步提交；提交信息写清楚「改了什么 + 为什么」。
3. **PR**：推送分支后开 Pull Request，标题概括改动，描述说明背景、改法、验证方式。
4. **main 保护**：`main` 只接受经过审核合并的 PR，不直推、不 force push。
5. **同步**：开工前先 `git fetch upstream main` 并基于最新代码，避免陈旧 diff。

## 建议流程

```text
git fetch upstream main
git checkout -b ai/<主题> upstream/main
# ... 改动、提交 ...
git push origin ai/<主题>
# 在 GitHub 上向 chen-xin-Liam/novus-os:main 发起 PR
```

## 备注

- 仓库 owner（chen-xin-Liam）在收到 PR 后负责审核与合并。
- 与 PR 相关的讨论请在 PR 评论区进行，保证可追溯。
