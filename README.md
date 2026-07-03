# DupliScan

DupliScan 是一个用于在 Windows 平台上查找并处理重复文件的小工具。它基于文件大小、头部哈希和采样哈希进行快速筛选，支持多线程扫描、导出 JSON、预览与交互式删除等功能。

主要特性
- 支持按扩展名、最小/最大文件大小过滤
- 快速模式（仅大小与头部哈希）和采样哈希以提高效率
- 多线程扫描，自动或手动指定线程数
- 导出扫描结果为 JSON
- 可交互或批量删除重复文件（支持预览模式）

先决条件
- Windows 10/11
- Visual Studio（推荐 2022 或更新版本，本文使用 Visual Studio Community 2026）

构建
1. 使用 Visual Studio 打开解决方案（或将源文件添加到新的 Visual C++ 控制台项目）。
2. 设置字符集为 Unicode（默认通常为 Unicode）。
3. 确保链接器包含 shlwapi.lib（代码中已通过 #pragma comment 引用）。
4. 编译并运行。

使用方法
在控制台中运行可执行文件，并传入要扫描的目录以及可选参数：

示例：
DupliScan "C:\\Users\\User\\Pictures" --ext=.jpg,.png --min-size=10 --quick --threads=4 --output=results.json

支持的选项（常用）
- --ext=.jpg,.png	按扩展名过滤
- --min-size=<KB>	最小文件大小（以 KB 为单位）
- --max-size=<KB>	最大文件大小（以 KB 为单位）
- --quick		快速模式（仅大小 + 头部哈希）
- --show-all		显示所有重复组（默认只显示前几组）
- --output=<file>	导出结果为 JSON
- --delete		删除重复文件（慎用）
- --keep=<method>	保留策略：oldest、newest、shortest_path、alphabetical
- --preview		仅预览（不实际删除）
- --interactive		逐个确认删除
- --threads=<N>		线程数（0=自动）
- --cache		启用增量扫描缓存（如实现）
- --verbose		详细输出

注意事项
- 删除文件操作危险且不可逆，请先使用 --preview 或 --output 导出结果并核对再执行删除。
- 程序尝试以 UTF-8/UTF-16 输出到控制台，若控制台显示异常，请确认控制台编码与字体支持。

许可
该项目的许可请参见仓库根目录的 LICENSE 文件。

联系方式
欢迎就功能增强、Bug 修复或提交 PR 提交 issue。
