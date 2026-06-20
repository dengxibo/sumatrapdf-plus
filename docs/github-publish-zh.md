# 将 Sumatra PDF Plus 源码发布到 GitHub

## 1. 在 GitHub 网站创建仓库

1. 登录 https://github.com
2. 右上角 **+** → **New repository**
3. 填写：
   - **Repository name**: `sumatrapdf-plus`（或你喜欢的名字）
   - **Public**（GPL 要求公开源码）
   - **不要**勾选 “Add a README”（本地已有）
4. 点 **Create repository**

## 2. 在本机推送（PowerShell）

把下面命令里的 `你的用户名` 换成你的 GitHub 用户名：

```powershell
cd c:\src\sumatrapdf

# 添加你自己的远程仓库（保留 origin 指向官方，方便以后对照）
git remote add plus https://github.com/你的用户名/sumatrapdf-plus.git

# 推送当前分支（分支名 yifan-darkmode，GitHub 上显示为 main）
git push -u plus yifan-darkmode:main
```

若提示需要登录，按 GitHub 要求使用 **Personal Access Token** 作为密码（Settings → Developer settings → Personal access tokens）。

## 3. 发布页上应写明的链接

在知乎 / 小众软件等下载说明里加上：

> 源代码（GPLv3）：https://github.com/你的用户名/sumatrapdf-plus

并说明：本程序基于 SumatraPDF 修改，非官方版本。

## 4. 可选：打 Release 标签

在 GitHub 仓库 **Releases** → **Draft a new release**：

- Tag: `v3.7-plus-test1`
- 上传你分发的 zip（主程序包；词典包若单独分发也可注明）

Release 不是 GPL 硬性要求，但方便用户对照「某个 zip 对应哪次源码」。

## 5. 常见问题

**Q: 必须 fork 官方仓库吗？**  
A: 不必。你可以推送到自己的独立仓库；readme 里注明 upstream 即可。

**Q: 词典 .dat 要上传吗？**  
A: 不必放进 GitHub，除非你有权分发且愿意托管。源码里已包含查词 **程序**；用户自备词典数据。

**Q: 官方会合并我的改动吗？**  
A: 这是独立 Plus 版；与官方无隶属关系。若有通用 bugfix，可另向 upstream 提 PR。
