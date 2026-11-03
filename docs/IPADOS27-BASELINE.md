# iPadOS 27：补接软件IOSurface表面的原登记函数

原SWContext::create_surface_from_iosurface成功返回前未调用Context::track_surface，而AsynchronousNode随后立刻push_surface，引发found != slist->end()。本版将原工厂的非NULL结果接回原track_surface，不跳过断言；原工厂/CF retain/锁/NULL路径保持。

通过已链接的上游native-text-fixup与payload.text_fixups，在原签名验证成功且原字节匹配后应用。保留工厂入口PACIBSP，重定向第二条STP到56B wrapper，原STP重放、双层PAC返回。wrapper位于__TEXT两个section之间的60B零padding；5条原字节守卫，原系统文件不落盘改写。

CA环境开关、job覆盖与未刷exec-env实验已退出series/参数/本地addon输入。参数恢复touch-split的922B，触控/Data seed/trust/显示requested保持。只做了一次必要机器码执行检查（实际原工厂+原track_surface、NULL/PAC/返回/寄存器），正式构建与最终镜像检查通过。原missing2/stubbed24精确集合保持；vendor_boot100462592B小于96MiB。

CRC 0485290E96ACC9B3；最终镜像SHA见SHA256SUMS。用户已确认上划进入系统正常、不再卡死；原始反馈见user-observation.json，运行时长未指定。部署状态以candidate-status.json/flash-result-*.json为准。

直接测试上划与进入桌面；若仍崩溃回fastboot取尾部，核text-fixup落到本次5条与新的实际崩溃栈。回退touch-split四分区或移除payload.text_fixups，Data保持。详情根docs/status/ipados27-unlock-compositor-2026-11-03.md。


## 2026-11-03 用户实机验收

用户原话：“这次正常了，不会卡死。记录一下吧”。上划卡死修复按用户可见结果通过，本release作为27线当前可交互基线。见user-observation.json/candidate-status.json与根milestones记录。

源码：`patches/userland/24A446/surface-track-wrapper.s`；精确守卫：同目录`surface-track-fixups.json`，与board.toml一致。
实机反馈：2026-11-03用户“这次正常了，不会卡死。记录一下吧”，上划卡死修复通过；当前CPU渲染，时长与其它应用未扩展验收。
