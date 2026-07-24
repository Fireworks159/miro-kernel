#!/bin/bash
#
# miro_kernel_opt install script
# 用法: 在内核源码根目录执行 bash /path/to/install.sh
#
set -e

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 检查是否在内核源码目录
if [ ! -f Makefile ] || [ ! -d kernel/sched ]; then
    echo "请在内核源码根目录执行此脚本"
    exit 1
fi

echo "=== miro kernel opt install ==="

# 1. 复制源码文件
cp "$SRC/include/linux/unfair_sched.h" include/linux/
cp "$SRC/include/linux/sun_power.h"    include/linux/
cp "$SRC/kernel/sched/unfair_sched.c"  kernel/sched/
cp "$SRC/kernel/sched/walt_sun.c"      kernel/sched/
cp "$SRC/kernel/power/sun_power.c"     kernel/power/
cp "$SRC/kernel/power/sun_idle.c"      kernel/power/
echo "[1/5] 源码文件已复制"

# 2. 追加 Makefile
grep -q "unfair_sched" kernel/sched/Makefile || {
cat >> kernel/sched/Makefile << 'EOF'

# miro kernel opt
obj-$(CONFIG_UNFAIR_SCHED) += unfair_sched.o
obj-$(CONFIG_WALT_SUN_ENHANCEMENT) += walt_sun.o
EOF
}
grep -q "sun_power" kernel/power/Makefile || {
cat >> kernel/power/Makefile << 'EOF'

# miro kernel opt
obj-$(CONFIG_SUN_POWER_OPTIMIZATION) += sun_power.o
obj-$(CONFIG_SUN_IDLE_OPTIMIZATION) += sun_idle.o
EOF
}
echo "[2/5] Makefile 已更新"

# 3. 追加 Kconfig
grep -q "Kconfig.miro_opt" init/Kconfig || {
echo 'source "Kconfig.miro_opt"' >> init/Kconfig
}
cat > Kconfig.miro_opt << 'EOF'
menu "miro Kernel Optimizations"

config UNFAIR_SCHED
	bool "Unfair Scheduler"
	default y
	depends on SCHED_WALT
	help
	  Foreground task priority boost scheduler.

config SUN_POWER_OPTIMIZATION
	bool "Sun Power Optimization"
	default y
	depends on PM && CPU_FREQ
	help
	  Power optimization for miro (SM8750).

config SUN_IDLE_OPTIMIZATION
	bool "Sun Idle Optimization"
	default y
	depends on SUN_POWER_OPTIMIZATION
	help
	  Aggressive idle state promotion.

config WALT_SUN_ENHANCEMENT
	bool "Walt Governor Enhancement"
	default y
	depends on SCHED_WALT && SUN_POWER_OPTIMIZATION
	help
	  Fast rise / slow drop frequency scaling.

config SUN_WAKELOCK_BLOCKER
	bool "Sun Wakelock Blocker"
	default y
	depends on SUN_POWER_OPTIMIZATION && PM_WAKELOCKS
	help
	  Block unwanted wakelocks.

endmenu
EOF
echo "[3/5] Kconfig 已更新"

# 4. 追加 defconfig
MIRO_CFG="arch/arm64/configs/vendor/miro_consolidate.config"
if [ -f "$MIRO_CFG" ] && ! grep -q "CONFIG_UNFAIR_SCHED" "$MIRO_CFG"; then
cat >> "$MIRO_CFG" << 'EOF'

# miro kernel opt
CONFIG_UNFAIR_SCHED=y
CONFIG_SUN_POWER_OPTIMIZATION=y
CONFIG_SUN_IDLE_OPTIMIZATION=y
CONFIG_WALT_SUN_ENHANCEMENT=y
CONFIG_SUN_WAKELOCK_BLOCKER=y
EOF
echo "[4/5] miro_consolidate.config 已更新"
else
echo "[4/5] defconfig 跳过（已存在或未找到）"
fi

# 5. 修改 fair.c —— 在 check_preempt_tick 中插入不公平调度逻辑
FAIR_C="kernel/sched/fair.c"
if [ -f "$FAIR_C" ] && ! grep -q "unfair_sched" "$FAIR_C"; then
    # 在文件头部添加 include
    sed -i '/#include <linux\/migrate.h>/a #include <linux\/unfair_sched.h>' "$FAIR_C"

    # 在 check_preempt_tick 函数的 ideal_runtime 赋值前插入 boost 逻辑
    # 找到 "ideal_runtime = sched_slice" 这行，在它前面插入
    sed -i '/ideal_runtime = sched_slice/i\
\tif (unfair_sched_is_favored(curr)) {\
\t\tunsigned long us_slice = unfair_sched_adjust_slice(curr, sched_slice(cfs_rq, curr));\
\t\tif (delta_exec < us_slice)\
\t\t\treturn;\
\t}' "$FAIR_C"

    # 在 wakeup_preempt_entity 中 vdiff > 0 的分支内追加 gran 调整
    sed -i '/if (vdiff > 0) {/a\\t\tgran += unfair_sched_adjust_wakeup_gran(task_of(se));' "$FAIR_C"

    echo "[5/5] fair.c 已修改"
else
    echo "[5/5] fair.c 跳过（已修改或未找到）"
fi

echo ""
echo "=== 完成 ==="
echo "编译:"
echo "  make miro_consolidate.config"
echo "  make -j\$(nproc)"
echo ""
echo "运行时控制:"
echo "  /proc/unfair_sched"
echo "  /proc/sun_power"
echo "  /proc/sun_idle"
echo "  /sys/kernel/walt_sun/"
