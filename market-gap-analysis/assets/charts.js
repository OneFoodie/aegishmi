// 市场功能差距分析报告 · 图表脚本
(function () {
  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var bg2 = style.getPropertyValue('--bg2').trim();
  var bad = style.getPropertyValue('--bad').trim();
  var na = style.getPropertyValue('--na').trim();

  // ---------- 各能力域状态分布 ----------
  var domains = ['EQ 设备接入', 'AC 采集显示', 'TP 测试程序', 'QA 质量过程', 'PL 产线人员', 'TR 追溯报告', 'SY 系统运维'];
  var done = [4, 7, 9, 7, 7, 8, 4];
  var part = [2, 0, 2, 0, 0, 0, 2];
  var gap = [0, 0, 3, 1, 0, 0, 1];
  var excluded = [0, 0, 1, 1, 2, 0, 0];

  var countLabel = {
    show: true,
    color: bg2,
    fontSize: 12,
    fontWeight: 'bold',
    formatter: function (p) { return p.value > 0 ? p.value : ''; }
  };

  var chartCoverage = echarts.init(document.getElementById('chart-coverage'), null, { renderer: 'svg' });
  chartCoverage.setOption({
    animation: false,
    color: [accent, accent2, bad, na],
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'shadow' },
      appendToBody: true
    },
    legend: {
      top: 0,
      left: 0,
      itemWidth: 12,
      itemHeight: 12,
      textStyle: { color: muted, fontSize: 12 }
    },
    grid: { left: 96, right: 24, top: 44, bottom: 28, containLabel: false },
    xAxis: {
      type: 'value',
      minInterval: 1,
      axisLine: { lineStyle: { color: rule } },
      axisTick: { show: false },
      axisLabel: { color: muted, fontSize: 12 },
      splitLine: { lineStyle: { color: rule, type: 'dashed' } }
    },
    yAxis: {
      type: 'category',
      data: domains,
      inverse: true,
      axisLine: { show: false },
      axisTick: { show: false },
      axisLabel: { color: ink, fontSize: 13 }
    },
    series: [
      {
        name: '已具备', type: 'bar', stack: 'total', data: done,
        barWidth: 18,
        label: countLabel
      },
      {
        name: '部分具备', type: 'bar', stack: 'total', data: part,
        label: countLabel
      },
      {
        name: '未覆盖', type: 'bar', stack: 'total', data: gap,
        label: countLabel
      },
      {
        name: '明确不做', type: 'bar', stack: 'total', data: excluded,
        itemStyle: { opacity: 0.55 },
        label: {
          show: true, color: ink, fontSize: 12,
          formatter: function (p) { return p.value > 0 ? p.value : ''; }
        }
      }
    ]
  });
  window.addEventListener('resize', function () { chartCoverage.resize(); });

  // ---------- 差距项优先级分布 ----------
  var priorityNames = ['不纳入', 'P3 按需', 'P2 条件具备', 'P1 立即补齐'];
  var priorityValues = [4, 4, 5, 1];
  var priorityColors = [na, accent, accent2, bad];

  var chartPriority = echarts.init(document.getElementById('chart-priority'), null, { renderer: 'svg' });
  chartPriority.setOption({
    animation: false,
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'shadow' },
      appendToBody: true,
      formatter: '{b}：{c} 项'
    },
    grid: { left: 110, right: 44, top: 12, bottom: 24, containLabel: false },
    xAxis: {
      type: 'value',
      minInterval: 1,
      axisLine: { show: false },
      axisTick: { show: false },
      axisLabel: { color: muted, fontSize: 12 },
      splitLine: { lineStyle: { color: rule, type: 'dashed' } }
    },
    yAxis: {
      type: 'category',
      data: priorityNames,
      axisLine: { show: false },
      axisTick: { show: false },
      axisLabel: { color: ink, fontSize: 13 }
    },
    series: [{
      type: 'bar',
      data: priorityValues.map(function (value, index) {
        return { value: value, itemStyle: { color: priorityColors[index], borderRadius: [0, 3, 3, 0] } };
      }),
      barWidth: 20,
      label: { show: true, position: 'right', color: ink, fontSize: 12, fontWeight: 'bold', formatter: '{c} 项' }
    }]
  });
  window.addEventListener('resize', function () { chartPriority.resize(); });
})();
