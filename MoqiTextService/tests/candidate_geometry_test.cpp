// candidate_geometry_test —— 候选窗几何纯函数回归：
//   1) InputRectScore（libIME2）：CUAS 幽灵点判废、前景窗内外加减分、
//      左上角小标记罚分、selection 先手；
//   2) CandidateClamp（MoqiTextService）：工作区四边夹取、右缘左移、
//      底部放不下翻到输入行上方、翻不了钉底。
// 这两层直接决定候选框钉在哪（候选框定位 saga 十二轮的终态逻辑），
// 行为逐字节锁定，防回归。
#include <windows.h>

#include <cstdio>

#include "../CandidateClamp.h"
#include <libIME2/src/InputRectScore.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char* name) {
  if (ok) {
    ++g_pass;
    std::printf("PASS %s\n", name);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", name);
  }
}

static RECT makeRect(int l, int t, int r, int b) {
  RECT rc{l, t, r, b};
  return rc;
}

int main() {
  using Ime::inputRectScore;

  // ---- inputRectScore ----
  const RECT foreground = makeRect(100, 100, 1000, 800);
  const RECT inside = makeRect(500, 500, 540, 520);  // 前景窗内，正常锚点
  check(inputRectScore(inside, &foreground, false) > 0,
        "score: normal anchor inside foreground is positive");
  check(inputRectScore(inside, &foreground, true) ==
            inputRectScore(inside, &foreground, false) + 1,
        "score: selection anchor gets +1 over composition");

  // CUAS 幽灵点：高度 ≤1px 直接判废，前景窗内奖励不得翻正（10-27 实测
  // wezterm 全屏 (1913,931,1914,931) 钉死屏幕右下的事故锚）。
  const RECT ghost = makeRect(1913, 931, 1914, 931);  // 高 0
  check(inputRectScore(ghost, &foreground, false) == -1000,
        "score: zero-height ghost rect is -1000");
  const RECT ghostInside = makeRect(500, 500, 501, 500);  // 在前景窗内但高 0
  check(inputRectScore(ghostInside, &foreground, true) == -1000,
        "score: inside-foreground does not revive ghost rect");
  const RECT thin = makeRect(500, 500, 540, 501);  // 高 1px
  check(inputRectScore(thin, &foreground, false) == -1000,
        "score: 1px-height rect is -1000");

  // 前景窗外罚分
  const RECT outside = makeRect(2000, 900, 2040, 920);
  check(inputRectScore(outside, &foreground, false) < 0,
        "score: anchor outside foreground is negative");

  // 宽 0（纯插入符列）与矮行罚分
  const RECT caretColumn = makeRect(500, 500, 500, 520);
  check(inputRectScore(caretColumn, &foreground, false) ==
            inputRectScore(inside, &foreground, false) - 10,
        "score: zero-width caret column -10 vs same-sized line");
  const RECT shortLine = makeRect(500, 500, 540, 506);  // 高 6 < 8
  check(inputRectScore(shortLine, &foreground, false) ==
            inputRectScore(inside, &foreground, false) - 10,
        "score: sub-8px height -10");

  // 负宽（废坐标）重罚（该矩形仍在前景窗内：+40 也不敌 -100）
  const RECT negative = makeRect(540, 500, 500, 520);
  check(inputRectScore(negative, &foreground, false) ==
            inputRectScore(inside, &foreground, false) - 100,
        "score: negative width heavily penalized");

  // 前景窗左上角小标记（宽 ≤4、高 ≤48、距左上角 ≤48px）：-80
  const RECT topLeftMarker = makeRect(104, 104, 106, 120);
  check(inputRectScore(topLeftMarker, &foreground, false) <=
            inputRectScore(inside, &foreground, false) - 80,
        "score: tiny top-left marker penalized");
  // 宽 5 的不算小标记
  const RECT wideMarker = makeRect(104, 104, 110, 120);
  check(inputRectScore(wideMarker, &foreground, false) ==
            inputRectScore(inside, &foreground, false),
        "score: width-5 rect is not a top-left marker");

  // 无前景窗矩形：几何罚分仍在
  check(inputRectScore(inside, nullptr, false) == 0,
        "score: no foreground rect, normal anchor = 0");
  check(inputRectScore(caretColumn, nullptr, false) == -10,
        "score: no foreground rect, zero-width -10");

  // ---- clampCandidateWindowPos ----
  const RECT work = makeRect(0, 0, 1920, 1040);
  const RECT line = makeRect(1000, 1000, 1060, 1020);  // 输入行在屏下方

  // 常规：锚点合法不动（整窗完全放得下）
  POINT pos{500, 500};
  Moqi::clampCandidateWindowPos(&pos, work, line, 200, 80);
  check(pos.x == 500 && pos.y == 500, "clamp: valid anchor untouched");

  // 左缘
  pos = {-50, 500};
  Moqi::clampCandidateWindowPos(&pos, work, line, 200, 80);
  check(pos.x == 8, "clamp: left edge -> work.left + 8");

  // 上缘
  pos = {500, -20};
  Moqi::clampCandidateWindowPos(&pos, work, line, 200, 80);
  check(pos.y == 8, "clamp: top edge -> work.top + 8");

  // 右缘：整窗左移
  pos = {1900, 500};
  Moqi::clampCandidateWindowPos(&pos, work, line, 200, 80);
  check(pos.x == 1920 - 8 - 200, "clamp: right edge slides left");

  // 右缘 + 窗比工作区宽：钉左缘
  pos = {1900, 500};
  const RECT narrowWork = makeRect(0, 0, 100, 1040);
  Moqi::clampCandidateWindowPos(&pos, narrowWork, line, 200, 80);
  check(pos.x == 8, "clamp: window wider than work pinned to left");

  // 底部放不下 + 输入行上方有空间 → 翻上方
  pos = {500, 1030};
  const RECT highLine = makeRect(500, 900, 560, 920);
  Moqi::clampCandidateWindowPos(&pos, work, highLine, 200, 80);
  check(pos.y == 900 - 8 - 80, "clamp: flips above input line");

  // 底部放不下 + 上方也没空间（输入行太靠上）→ 钉工作区底
  pos = {500, 1030};
  const RECT topLine = makeRect(500, 0, 560, 20);
  Moqi::clampCandidateWindowPos(&pos, work, topLine, 200, 80);
  check(pos.y == 1040 - 8 - 80, "clamp: cannot flip, pinned to bottom");

  // 尺寸未知（0）：左/上缘的点夹取仍生效；右/下缘不按整窗处理
  // （这两条锁住 width>0/height>0 守卫——删守卫后右缘会把 x 夹到 1912）。
  pos = {-50, 500};
  Moqi::clampCandidateWindowPos(&pos, work, line, 0, 0);
  check(pos.x == 8 && pos.y == 500,
        "clamp: zero size still point-clamps left edge");
  pos = {1915, 1030};
  Moqi::clampCandidateWindowPos(&pos, work, line, 0, 0);
  check(pos.x == 1915 && pos.y == 1030,
        "clamp: zero size skips window-width right/bottom clamp");

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
