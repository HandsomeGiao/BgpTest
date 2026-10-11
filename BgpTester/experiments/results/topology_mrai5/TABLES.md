# MRAI 5 秒拓扑实验数据表

本表由原始结果自动生成。主表采用延迟倍率 1、撤销绕过 MRAI；箭头表示标准 BGP → full。

## 各场景的首个扰动阶段

| 场景 | 节点/边 | 观察点中间路径 | 全网中间路径 | 全网失效 AS 路径选择 | 队列清空 s | UPDATE 数 | 有物理通路但代理不可用 s |
|---|---:|---:|---:|---:|---:|---:|---:|
| shared_loss | 8/9 | 1 → 0 | 1 → 0 | 1 → 0 | 0.081 → 0.086 | 4 → 44 | 0.081 → 0.002 |
| local_link_loss | 8/9 | 0 → 0 | 0 → 0 | 0 → 0 | 0.001 → 0.090 | 1 → 19 | 0.001 → 0.001 |
| export_only | 8/9 | 0 → 0 | 0 → 0 | 0 → 0 | 0.001 → 0.083 | 1 → 20 | 0.000 → 0.000 |
| rapid_recovery | 8/9 | 1 → 1 | 1 → 1 | 1 → 0 | 0.094 → 0.176 | 9 → 129 | 0.014 → 0.002 |
| origin_withdraw | 8/9 | 1 → 0 | 1 → 0 | 1 → 0 | 0.086 → 0.088 | 9 → 94 | 0.000 → 0.000 |
| reroute | 8/10 | 1 → 0 | 1 → 0 | 1 → 0 | 0.081 → 0.163 | 5 → 98 | 0.000 → 0.000 |
| metadata_gap | 6/7 | 2 → 1 | 5 → 3 | 1 → 0 | 5.081 → 5.163 | 13 → 160 | 0.000 → 4.988 |
| multi_origin | 8/9 | 1 → 0 | 1 → 0 | 1 → 0 | 0.086 → 0.091 | 6 → 66 | 0.081 → 0.002 |
| ibgp_border | 4/3 | 0 → 0 | 0 → 0 | — | 0.020 → 0.045 | 3 → 18 | 0.000 → 0.000 |
| ibgp_origin | 3/2 | 0 → 0 | 0 → 0 | — | 0.015 → 0.040 | 2 → 10 | 0.000 → 0.000 |
| fanout | 14/21 | 7 → 0 | 7 → 0 | 7 → 0 | 0.072 → 0.077 | 16 → 356 | 0.072 → 0.002 |
| mrai_cascade | 9/10 | 1 → 0 | 2 → 0 | 2 → 0 | 5.003 → 0.086 | 6 → 85 | 0.081 → 0.002 |
| chain_withdraw | 8/7 | 0 → 0 | 0 → 0 | 0 → 0 | 0.070 → 0.150 | 7 → 70 | 0.000 → 0.000 |
| star_withdraw | 9/8 | 0 → 0 | 0 → 0 | 0 → 0 | 0.010 → 0.040 | 8 → 144 | 0.000 → 0.000 |
| tree_withdraw | 15/14 | 0 → 0 | 0 → 0 | 0 → 0 | 0.030 → 0.100 | 14 → 380 | 0.000 → 0.000 |
| independent_failure | 4/4 | 0 → 0 | 0 → 0 | 0 → 0 | 0.001 → 0.042 | 1 → 11 | 0.001 → 0.001 |
| ring_withdraw | 8/8 | 1 → 0 | 4 → 0 | 4 → 0 | 0.124 → 0.136 | 17 → 91 | 0.000 → 0.000 |
| ring_failure | 8/8 | 0 → 0 | 0 → 0 | 0 → 0 | 0.066 → 0.211 | 7 → 79 | 0.047 → 0.047 |
| grid_withdraw | 9/12 | 3 → 0 | 22 → 0 | 22 → 0 | 0.073 → 0.076 | 48 → 184 | 0.000 → 0.000 |
| grid_failure | 9/12 | 0 → 0 | 2 → 0 | 2 → 0 | 5.041 → 0.075 | 18 → 228 | 0.025 → 0.025 |
| clique6_withdraw | 6/15 | 4 → 0 | 23 → 0 | 23 → 0 | 0.167 → 0.081 | 60 → 160 | 0.000 → 0.000 |
| clique8_withdraw | 8/28 | 11 → 0 | 75 → 0 | 75 → 0 | 15.138 → 0.080 | 303 → 392 | 0.000 → 0.000 |
| clique_cut | 7/16 | 4 → 0 | 23 → 0 | 23 → 0 | 0.167 → 0.081 | 60 → 185 | 0.000 → 0.000 |
| tiered_failure | 6/9 | 0 → 0 | 0 → 0 | 0 → 0 | 0.031 → 0.064 | 5 → 59 | 0.000 → 0.000 |
| layered_failure | 13/21 | 3 → 0 | 9 → 0 | 9 → 0 | 0.066 → 0.071 | 18 → 253 | 0.066 → 0.004 |
| fanout2 | 8/9 | 1 → 0 | 1 → 0 | 1 → 0 | 0.012 → 0.017 | 4 → 44 | 0.012 → 0.002 |
| fanout4 | 10/13 | 3 → 0 | 3 → 0 | 3 → 0 | 0.032 → 0.037 | 8 → 116 | 0.032 → 0.002 |
| fanout16 | 22/37 | 15 → 0 | 15 → 0 | 15 → 0 | 0.152 → 0.157 | 32 → 1220 | 0.152 → 0.002 |
| fanout32 | 38/69 | 31 → 0 | 31 → 0 | 31 → 0 | 0.312 → 0.317 | 64 → 4484 | 0.312 → 0.002 |
| fanout8_reverse | 14/21 | 1 → 0 | 1 → 0 | 1 → 0 | 0.072 → 0.077 | 16 → 356 | 0.072 → 0.002 |
| fanout8_equal | 14/21 | 7 → 0 | 7 → 0 | 7 → 0 | 0.011 → 0.032 | 16 → 370 | 0.011 → 0.011 |
| fanout8_shuffled_7 | 14/21 | 2 → 0 | 2 → 0 | 2 → 0 | 0.072 → 0.077 | 16 → 356 | 0.072 → 0.002 |
| fanout8_shuffled_19 | 14/21 | 2 → 0 | 2 → 0 | 2 → 0 | 0.072 → 0.077 | 16 → 356 | 0.072 → 0.002 |
| fanout8_shuffled_42 | 14/21 | 4 → 0 | 4 → 0 | 4 → 0 | 0.072 → 0.077 | 16 → 356 | 0.072 → 0.002 |

## 机制消融

延迟倍率 1、撤销绕过 MRAI；取首个扰动阶段。

| 场景 | 机制 | 全网中间路径 | 全网失效 AS 路径选择 | 队列清空 s | UPDATE 数 | 独立状态 UPDATE 数 |
|---|---|---:|---:|---:|---:|---:|
| shared_loss | standard | 1 | 1 | 0.081 | 4 | 0 |
| shared_loss | path_only | 1 | 1 | 0.081 | 8 | 0 |
| shared_loss | local | 1 | 1 | 0.160 | 20 | 12 |
| shared_loss | full | 0 | 0 | 0.086 | 44 | 36 |
| fanout32 | standard | 31 | 31 | 0.312 | 64 | 0 |
| fanout32 | path_only | 31 | 31 | 0.312 | 128 | 0 |
| fanout32 | local | 31 | 31 | 0.622 | 1280 | 1152 |
| fanout32 | full | 0 | 0 | 0.317 | 4484 | 4356 |
| clique8_withdraw | standard | 75 | 75 | 15.138 | 303 | 0 |
| clique8_withdraw | path_only | 24 | 24 | 0.113 | 112 | 0 |
| clique8_withdraw | local | 0 | 0 | 0.078 | 105 | 56 |
| clique8_withdraw | full | 0 | 0 | 0.080 | 392 | 343 |
| layered_failure | standard | 9 | 9 | 0.066 | 18 | 0 |
| layered_failure | path_only | 9 | 9 | 0.066 | 36 | 0 |
| layered_failure | local | 10 | 10 | 0.122 | 107 | 71 |
| layered_failure | full | 0 | 0 | 0.071 | 253 | 221 |
| mrai_cascade | standard | 2 | 2 | 5.003 | 6 | 0 |
| mrai_cascade | path_only | 2 | 2 | 5.003 | 12 | 0 |
| mrai_cascade | local | 2 | 2 | 5.004 | 30 | 18 |
| mrai_cascade | full | 0 | 0 | 0.086 | 85 | 75 |
| metadata_gap | standard | 5 | 1 | 5.081 | 13 | 0 |
| metadata_gap | path_only | 4 | 0 | 5.081 | 26 | 0 |
| metadata_gap | local | 6 | 1 | 5.160 | 66 | 36 |
| metadata_gap | full | 3 | 0 | 5.163 | 160 | 132 |

## 全部成对比较

lower/equal/higher 分别表示 full 相对标准 BGP 更小、相同、更大。

| 指标 | 首次扰动 lower/equal/higher | 含恢复阶段 lower/equal/higher |
|---|---:|---:|
| network_intermediate_paths | 94 / 42 / 0 | 94 / 168 / 2 |
| network_invalid_as_path_selections | 92 / 44 / 0 | 92 / 172 / 0 |
| observer_intermediate_paths | 92 / 44 / 0 | 92 / 170 / 2 |
| observer_path_changes | 84 / 44 / 8 | 84 / 166 / 14 |
| observer_last_path_change_ms | 84 / 50 / 2 | 84 / 178 / 2 |
| convergence_ms | 26 / 0 / 110 | 26 / 24 / 214 |
| updates_sent | 0 / 0 / 136 | 0 / 0 / 264 |
| announcements_sent | 24 / 84 / 28 | 24 / 128 / 112 |
| withdrawals_sent | 16 / 28 / 92 | 16 / 136 / 112 |
| control_updates_sent | 0 / 0 / 136 | 0 / 4 / 260 |
| proxy_unreachable_ms | 72 / 30 / 34 | 72 / 158 / 34 |
| proxy_no_route_ms | 8 / 86 / 42 | 8 / 214 / 42 |
| proxy_unreachable_while_origin_connected_ms | 56 / 74 / 6 | 56 / 202 / 6 |
| estimated_extension_bytes | 0 / 0 / 136 | 0 / 0 / 264 |

## 拓扑清单

| 场景 | 类型 | 观察点 | 构造和扰动 | 拓扑 |
|---|---|---|---|---|
| shared_loss | shared_dependency | X | 共同依赖失效，有独立备用 | [JSON](../../../topo/versioned_mrai5/shared-dependency.json) |
| local_link_loss | shared_dependency | X | 单条分支链路失效，另一分支仍可用 | [JSON](../../../topo/versioned_mrai5/shared-dependency.json) |
| export_only | shared_dependency | X | 仅撤销 A 对 X 的导出，保留共享状态 | [JSON](../../../topo/versioned_mrai5/shared-dependency.json) |
| rapid_recovery | shared_dependency | X | 共享依赖断开后 3 ms 恢复 | [JSON](../../../topo/versioned_mrai5/shared-dependency.json) |
| origin_withdraw | shared_dependency | X | 唯一源撤销目标前缀，另一前缀继续发布 | [JSON](../../../topo/versioned_mrai5/shared-dependency.json) |
| reroute | reroute | X | 共同依赖节点自身绕行 | [JSON](../../../topo/versioned_mrai5/reroute.json) |
| metadata_gap | metadata_gap | X | 退役事实先到，新正向声明等待 MRAI | [JSON](../../../topo/versioned_mrai5/reroute-without-independent-backup.json) |
| multi_origin | multi_origin | X | 撤销双起源中的一个 | [JSON](../../../topo/versioned_mrai5/multiple-origins.json) |
| ibgp_border | ibgp | X | 同 AS 多边界，经普通 iBGP 传播 | [JSON](../../../topo/versioned_mrai5/ibgp-border.json) |
| ibgp_origin | ibgp | X | 空 AS_PATH 的 iBGP 本地起源 | [JSON](../../../topo/versioned_mrai5/ibgp-origin.json) |
| fanout | fanout | X | 8 分支引用相同旧依赖 | [JSON](../../../topo/versioned_mrai5/eight-shared-branches.json) |
| mrai_cascade | cascade | Y | 中间路径发布触发下游一轮 MRAI | [JSON](../../../topo/versioned_mrai5/mrai-exploration-cascade.json) |
| chain_withdraw | chain | R7 | 8 节点链，唯一传播路径 | [JSON](../../../topo/versioned_mrai5/chain-8.json) |
| star_withdraw | star | R8 | 单中心 8 叶节点，无替代路径 | [JSON](../../../topo/versioned_mrai5/star-9.json) |
| tree_withdraw | tree | R14 | 15 节点二叉树，分叉但不重汇合 | [JSON](../../../topo/versioned_mrai5/tree-15.json) |
| independent_failure | independent | X | 首选分支断开，独立备用有效 | [JSON](../../../topo/versioned_mrai5/independent-diamond.json) |
| ring_withdraw | ring | R3 | 8 节点环，唯一源撤销 | [JSON](../../../topo/versioned_mrai5/ring-8.json) |
| ring_failure | ring | R3 | 8 节点环单边断开，可反向绕行 | [JSON](../../../topo/versioned_mrai5/ring-8.json) |
| grid_withdraw | grid | G22 | 3×3 网格，多个重叠替代路径 | [JSON](../../../topo/versioned_mrai5/grid-3x3.json) |
| grid_failure | grid | G20 | 3×3 网格，起源的一条邻边失效 | [JSON](../../../topo/versioned_mrai5/grid-3x3.json) |
| clique6_withdraw | clique | R5 | 6 节点全互联，唯一源撤销 | [JSON](../../../topo/versioned_mrai5/clique-6.json) |
| clique8_withdraw | clique | R7 | 8 节点全互联，唯一源撤销 | [JSON](../../../topo/versioned_mrai5/clique-8.json) |
| clique_cut | clique | R5 | 6 节点全互联经单一出口接起源，出口断开 | [JSON](../../../topo/versioned_mrai5/clique-shared-neck.json) |
| tiered_failure | tiered | X | 双上游加 peer，执行商业偏好与 valley-free | [JSON](../../../topo/versioned_mrai5/tiered-policy.json) |
| layered_failure | layered | X | 两层 3×3 分支反复合流，共享出口断开 | [JSON](../../../topo/versioned_mrai5/layered-3x3.json) |
| fanout2 | fanout | X | 2 分支，退役按偏好先后到达 | [JSON](../../../topo/versioned_mrai5/fanout-2-ordered.json) |
| fanout4 | fanout | X | 4 分支，退役按偏好先后到达 | [JSON](../../../topo/versioned_mrai5/fanout-4-ordered.json) |
| fanout16 | fanout | X | 16 分支，退役按偏好先后到达 | [JSON](../../../topo/versioned_mrai5/fanout-16-ordered.json) |
| fanout32 | fanout | X | 32 分支，退役按偏好先后到达 | [JSON](../../../topo/versioned_mrai5/fanout-32-ordered.json) |
| fanout8_reverse | fanout_order | X | 8 分支延迟排列 reverse | [JSON](../../../topo/versioned_mrai5/fanout-8-reverse.json) |
| fanout8_equal | fanout_order | X | 8 分支延迟排列 equal | [JSON](../../../topo/versioned_mrai5/fanout-8-equal.json) |
| fanout8_shuffled_7 | fanout_order | X | 8 分支延迟排列 shuffled，seed=7 | [JSON](../../../topo/versioned_mrai5/fanout-8-shuffled-7.json) |
| fanout8_shuffled_19 | fanout_order | X | 8 分支延迟排列 shuffled，seed=19 | [JSON](../../../topo/versioned_mrai5/fanout-8-shuffled-19.json) |
| fanout8_shuffled_42 | fanout_order | X | 8 分支延迟排列 shuffled，seed=42 | [JSON](../../../topo/versioned_mrai5/fanout-8-shuffled-42.json) |

原始完整序列见 [raw_results.json.gz](raw_results.json.gz)；CSV 保留全部延迟倍率、撤销模式和恢复阶段。
唯一源永久撤销时，后续无路由是预期状态；proxy_no_route_ms 和 proxy_unreachable_ms 的积分终点为各自队列清空时刻，不能跨机制解释为业务中断差异。
主表改用尚有物理路径通向有效起源时的代理不可用时长；该值依然不是实际丢包测量。
