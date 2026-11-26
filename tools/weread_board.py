#!/usr/bin/env python3
"""微信读书荐书板生成器（本机旁路 / GitHub Actions）。

流程：
  1. 拉阅读喜好画像（/readdata/detail 等）——底层口味信号
  2. DeepSeek 根据画像提出检索词 / 书名（选题发现，不走「为你推荐」）
  3. 微信读书 /store/search 把检索落地成真实书目候选
  4. DeepSeek 从候选中定一本；热门划线/简介仍走微信读书；荐读由 AI 写（谈书本身）
  5. 写出 JSON 到 examples/ 与 docs/（GitHub Pages）；Tab5 GET 片单 URL

定时：GitHub Actions 每天 07:00（北京时间）或本机 --serve；Tab5 本地轮播。

环境变量：
  WEREAD_API_KEY       必填（wrk-...）
  DEEPSEEK_API_KEY      必填（用于选书与写荐读）
  DEEPSEEK_BASE_URL     必填（中转站根地址，如 https://xxx.com/v1）
  DEEPSEEK_MODEL        默认 deepseek-chat
  WEREAD_BOARD_PORT     默认 8788（仅 --serve）
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import random
import re
import socketserver
import ssl
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "examples"
DOCS_DIR = ROOT / "docs"
SETTINGS_PATH = DOCS_DIR / "settings.json"
OUT = OUT_DIR / "weread_book.json"
DAY_OUT = OUT_DIR / "weread_day.json"
HISTORY = OUT_DIR / "weread_history.json"
DOCS_DAY = DOCS_DIR / "weread_day.json"
DOCS_BOOK = DOCS_DIR / "weread_book.json"
DOCS_HISTORY = DOCS_DIR / "weread_history.json"
SKILL_VERSION = "1.0.3"
GATEWAY = "https://i.weread.qq.com/api/agent/gateway"
TZ = timezone(timedelta(hours=8))
RETRY_WINDOW_MIN = 90  # 7 点后窗口内可重试生成片单
RETRY_GAP_SEC = 120
PORT = int(os.environ.get("WEREAD_BOARD_PORT", "8788"))
LLM_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat").strip() or "deepseek-chat"


def load_settings() -> dict:
    if not SETTINGS_PATH.is_file():
        return {}
    try:
        data = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


_SETTINGS = load_settings()
DAILY_HOUR = int(_SETTINGS.get("daily_hour") or 7)
PLAYLIST_SIZE = max(1, min(5, int(_SETTINGS.get("playlist_size") or 5)))
HISTORY_DAYS = max(1, int(_SETTINGS.get("history_days") or 7))

# 荐书板只要正经可读之书：挡期刊/童书等非阅读向，以及快阅读包装。
BLOCKED_CATEGORIES = {
    "期刊杂志",
    "童书",
    "漫画",
    "漫画绘本",
    "少儿",
    "儿童文学",
}
# 书名命中即视为快阅读 / 速成包装
FAST_READ_TITLE_RE = re.compile(
    r"半小时|"
    r"轻思维|轻科学|轻经济|轻科技|轻纪实|轻阅读|"
    r"一本书读懂|一本书精读|半小时精读|半小时讲透|半小时读懂|"
    r"[（(]轻[^）)]*[）)]"
)

# Tab5 当前 efontCN 可绘字符（BMP bitset，由 tools/extract_efont_charset.py 生成）
_EFONT_BITSET_PATH = Path(__file__).resolve().parent / "efont_cn_allowed.bin"
_EFONT_BITSET: bytes | None = None
# ASCII 可显示 + 常见空白；中日韩标点若字库没有会在 bitset 外被拦
_ALWAYS_OK = set(" \t\n\r　")


def _efont_bitset() -> bytes:
    global _EFONT_BITSET
    if _EFONT_BITSET is None:
        if not _EFONT_BITSET_PATH.is_file():
            raise RuntimeError(f"缺少字库白名单 {_EFONT_BITSET_PATH}")
        _EFONT_BITSET = _EFONT_BITSET_PATH.read_bytes()
    return _EFONT_BITSET


def efont_char_ok(ch: str) -> bool:
    if not ch:
        return True
    if ch in _ALWAYS_OK:
        return True
    cp = ord(ch)
    if cp < 0x20:
        return False
    if cp <= 0xFFFF:
        bs = _efont_bitset()
        return bool(bs[cp >> 3] & (1 << (cp & 7)))
    return False


def efont_unsupported(text: str) -> list[str]:
    bad: list[str] = []
    seen: set[str] = set()
    for ch in text or "":
        if ch in seen:
            continue
        if not efont_char_ok(ch):
            bad.append(ch)
            seen.add(ch)
    return bad


def efont_safe(text: str) -> bool:
    return not efont_unsupported(text)


def scrub_efont(text: str) -> str:
    """去掉屏上会变成方框的字；保留可显示字符。"""
    return "".join(ch for ch in (text or "") if efont_char_ok(ch))


def llm_chat_url() -> str:
    """中转站根地址 + /chat/completions。支持带或不带 /v1。"""
    base = os.environ.get("DEEPSEEK_BASE_URL", "").strip().rstrip("/")
    if not base:
        raise SystemExit(
            "请设置 DEEPSEEK_BASE_URL（中转站地址，如 https://your-relay.com/v1）"
        )
    if base.endswith("/chat/completions"):
        return base
    return f"{base}/chat/completions"

# Tab5 荐读正文 28px：约 35 字/行；上限 4 行、下限 3.5 行。
RECOMMEND_CHARS_PER_LINE = 35
RECOMMEND_MAX_CHARS = int(RECOMMEND_CHARS_PER_LINE * 4)  # 140
RECOMMEND_MIN_CHARS = int(RECOMMEND_CHARS_PER_LINE * 3.5)  # 122
_LEADING_PUNCT = "，、。！？；：》」』）】…\"'）,.;:!?)]}"
_RECOMMEND_FILLERS = (
    "把含糊处读明白，判断会更稳一点。",
    "值得放在手边，有空就翻两页。",
    "语气清楚，读完仍想再翻一翻。",
    "材料讲得实在，不靠空洞口号撑场。",
)

_SSL_CTX: ssl.SSLContext | None = None


def _ssl_ctx() -> ssl.SSLContext:
    global _SSL_CTX
    if _SSL_CTX is not None:
        return _SSL_CTX
    try:
        import certifi  # type: ignore

        _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
    except Exception:
        _SSL_CTX = ssl._create_unverified_context()
    return _SSL_CTX


def now_bj() -> datetime:
    return datetime.now(TZ)


def now_str() -> str:
    return now_bj().strftime("%Y-%m-%d %H:%M:%S")


def api_key() -> str:
    key = os.environ.get("WEREAD_API_KEY", "").strip()
    if not key:
        raise SystemExit("请设置 WEREAD_API_KEY")
    return key


def llm_key() -> str:
    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        raise SystemExit("请设置 DEEPSEEK_API_KEY（用于选书与写荐读）")
    return key


def http_json(url: str, body: dict, headers: dict, timeout: int = 60) -> dict:
    req = urllib.request.Request(
        url,
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout, context=_ssl_ctx()) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    return data if isinstance(data, dict) else {}


def weread(api_name: str, **params) -> dict:
    data = http_json(
        GATEWAY,
        {"api_name": api_name, "skill_version": SKILL_VERSION, **params},
        {
            "Authorization": f"Bearer {api_key()}",
            "Content-Type": "application/json",
        },
        timeout=45,
    )
    if data.get("errcode") not in (None, 0):
        raise RuntimeError(f"{api_name} errcode={data.get('errcode')} {data.get('errmsg')}")
    return data


def llm_chat(system: str, user: str, temperature: float = 0.5) -> str:
    data = http_json(
        llm_chat_url(),
        {
            "model": LLM_MODEL,
            "temperature": temperature,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
        },
        {
            "Authorization": f"Bearer {llm_key()}",
            "Content-Type": "application/json",
        },
        timeout=90,
    )
    try:
        return (data["choices"][0]["message"]["content"] or "").strip()
    except (KeyError, IndexError, TypeError) as exc:
        raise RuntimeError(f"DeepSeek 回包异常: {data}") from exc


def load_history() -> list[dict]:
    if not HISTORY.is_file():
        return []
    try:
        return json.loads(HISTORY.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []


def _atomic_write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)


def save_history(entries: list[dict]) -> None:
    clipped = entries[-200:]
    _atomic_write_json(HISTORY, clipped)
    _atomic_write_json(DOCS_HISTORY, clipped)


def recent_ids(entries: list[dict]) -> set[str]:
    cutoff = now_bj() - timedelta(days=HISTORY_DAYS)
    ids: set[str] = set()
    for e in entries:
        try:
            ts = datetime.strptime(e.get("at", ""), "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ)
        except ValueError:
            continue
        if ts >= cutoff and e.get("bookId"):
            ids.add(str(e["bookId"]))
    return ids


def series_key(title: str) -> str:
    """同系列去重键：近期荐过「半小时讲透*」则不再连荐该系列。"""
    t = (title or "").strip()
    m = re.match(r"^(半小时讲透)", t)
    return m.group(1) if m else ""


def recent_series(entries: list[dict]) -> set[str]:
    cutoff = now_bj() - timedelta(days=HISTORY_DAYS)
    keys: set[str] = set()
    for e in entries:
        try:
            ts = datetime.strptime(e.get("at", ""), "%Y-%m-%d %H:%M:%S").replace(tzinfo=TZ)
        except ValueError:
            continue
        if ts < cutoff:
            continue
        key = series_key(e.get("title") or "")
        if key:
            keys.add(key)
    return keys


def shelf_ids() -> set[str]:
    data = weread("/shelf/sync")
    ids = {str(b.get("bookId")) for b in (data.get("books") or []) if b.get("bookId")}
    for a in data.get("albums") or []:
        aid = (a.get("albumInfo") or {}).get("albumId")
        if aid:
            ids.add(str(aid))
    return ids


def is_fast_or_non_reading(book: dict) -> bool:
    """期刊/童书/漫画等，或书名呈快阅读包装。"""
    title = book.get("title") or ""
    cat = (book.get("category") or "").replace("／", "/").replace("-", "/")
    cat_parts = {p.strip() for p in cat.split("/") if p.strip()}
    if cat_parts & BLOCKED_CATEGORIES:
        return True
    # 有的回包 category 为空，用刊期样式兜底
    if re.search(r"第\s*\d+\s*期", title) or re.search(r"\d{4}年第", title):
        return True
    if FAST_READ_TITLE_RE.search(title):
        return True
    return False


def _acceptable(book: dict, on_shelf: set[str], seen: set[str], blocked_series: set[str]) -> bool:
    bid = str(book.get("bookId") or "")
    title = book.get("title") or ""
    author = book.get("author") or ""
    if not bid or bid in on_shelf or bid in seen:
        return False
    if not book.get("cover") or not title:
        return False
    if is_fast_or_non_reading(book):
        return False
    key = series_key(title)
    if key and key in blocked_series:
        return False
    # 书名/作者含 efont 缺字（如「玥」）则跳过，避免屏上方框
    bad = efont_unsupported(title) or efont_unsupported(author)
    if bad:
        return False
    return True


def search_ebooks(keyword: str, count: int = 15) -> list[dict]:
    """/store/search scope=10 电子书，压平为与 enrich 兼容的 book dict。"""
    kw = (keyword or "").strip()
    if not kw:
        return []
    try:
        data = weread("/store/search", keyword=kw, scope=10, count=count)
    except Exception as exc:  # noqa: BLE001
        print(f"search fail [{kw}]: {exc}")
        return []
    out: list[dict] = []
    for group in data.get("results") or []:
        for row in group.get("books") or []:
            info = row.get("bookInfo") or {}
            if int(row.get("soldout") or info.get("soldout") or 0) == 1:
                continue
            bid = info.get("bookId") or row.get("bookId")
            title = info.get("title") or ""
            if not bid or not title:
                continue
            out.append(
                {
                    "bookId": str(bid),
                    "title": title,
                    "author": info.get("author") or "",
                    "cover": info.get("cover") or "",
                    "intro": info.get("intro") or "",
                    "category": info.get("category") or "",
                    "newRating": row.get("newRating") if row.get("newRating") is not None else info.get("newRating"),
                    "newRatingCount": row.get("newRatingCount")
                    if row.get("newRatingCount") is not None
                    else info.get("newRatingCount"),
                    "newRatingDetail": row.get("newRatingDetail") or info.get("newRatingDetail"),
                    "reason": f"检索「{kw}」",
                    "_query": kw,
                }
            )
    return out


# 每日片单五轨：强制题材分散，历史类整天最多 1 本
PLAYLIST_LANES = (
    {
        "id": "fiction_cn",
        "label": "中文小说/文学",
        "family": "fiction",
        "hint": "中文长篇小说或文学，不要历史非虚构、不要人物传记",
        "seeds": ["当代小说 经典", "余华 小说", "王小波 小说", "张爱玲 小说"],
    },
    {
        "id": "fiction_world",
        "label": "翻译文学",
        "family": "world",
        "hint": "外国文学或翻译小说，可略偏严肃，不要中国历史传记",
        "seeds": ["格雷厄姆·格林 小说", "萨拉马戈 小说", "翻译文学 经典", "马尔克斯 小说"],
    },
    {
        "id": "essay",
        "label": "随笔/思想",
        "family": "essay",
        "hint": "随笔、思想、文化评论，不要通史、断代史、人物长传",
        "seeds": ["随笔 经典", "思想 随笔", "文化评论", "木心 文学"],
    },
    {
        "id": "science",
        "label": "科学/社会/艺术",
        "family": "science",
        "hint": "科学、社会观察、艺术或自然写作，明确避开中国史/世界史传记",
        "seeds": ["科学 非虚构", "自然写作", "社会学 经典", "艺术 史 入门"],
    },
    {
        "id": "history",
        "label": "历史非虚构（本日唯一）",
        "family": "history",
        "hint": "历史非虚构或人物传记，本日片单仅此一本历史向",
        "seeds": ["历史 非虚构", "人物传记 经典", "微观史"],
    },
)


def book_family(book: dict) -> str:
    """粗分题材族，用于片单去同质。"""
    cat = f"{book.get('category') or ''} {book.get('title') or ''}"
    if any(k in cat for k in ("历史", "传记", "纪实", "史学", "通史", "断代")):
        return "history"
    if any(k in cat for k in ("科幻", "科学", "自然", "社会", "艺术", "心理")):
        return "science"
    if any(k in cat for k in ("随笔", "散文", "思想", "哲学", "评论")):
        return "essay"
    if any(k in cat for k in ("外国", "翻译", "世界文学")):
        return "world"
    if any(k in cat for k in ("小说", "文学", "诗歌", "戏剧")):
        return "fiction"
    # 作者名启发：洋名分隔点常见于翻译书
    author = book.get("author") or ""
    if "·" in author or "." in author or any(ch.isascii() and ch.isalpha() for ch in author):
        return "world"
    return "other"


def ai_discover_queries(
    taste: dict,
    recent_titles: list[str],
    *,
    lane: dict | None = None,
    avoid_families: list[str] | None = None,
) -> dict:
    """DeepSeek 从喜好底层提出检索词与可选书名；不依赖「为你推荐」。"""
    lane_line = ""
    if lane:
        lane_line = (
            f"本次必须落在赛道「{lane['label']}」：{lane['hint']}。"
            "检索词与书名都要服务该赛道，不要偏到已占满的题材。"
        )
    avoid_line = ""
    if avoid_families:
        avoid_line = f"严禁再推荐这些题材族：{', '.join(avoid_families)}。"
    system = (
        "你是私人书房选题顾问。根据读者真实阅读画像，提出下一步该读什么方向。"
        "不要迎合平台推荐流；要主动发现口味深处的延续与适度拓展。"
        "禁止期刊杂志、童书、漫画、半小时/轻系列、一本书读懂等快阅读。"
        f"{lane_line}{avoid_line}"
        "检索词与书名只用常用汉字、字母、数字与常规标点；避免生僻字、罕用人名用字（如玥、龘等），"
        "以免墨水屏点阵字库缺字显示方框。"
        "必须只输出 JSON："
        '{"queries":["检索词1",...],"titles":["可选精确书名",...],"angle":"不超过40字选题思路"}。'
        "queries 4 到 6 条，短而可搜（可含作者+题材、时代+体裁、经典书系等）；"
        "titles 0 到 3 本，须是你认为读者可能喜欢的正经书，且尽量不与 recent 重复。"
    )
    user = json.dumps(
        {
            "taste": taste,
            "recent_recommended_titles": recent_titles[-10:],
            "lane": lane,
            "avoid_families": avoid_families or [],
        },
        ensure_ascii=False,
    )
    raw = llm_chat(system, user, temperature=0.55)
    m = re.search(r"\{[\s\S]*\}", raw)
    if not m:
        raise RuntimeError(f"选题 JSON 缺失: {raw[:160]}")
    data = json.loads(m.group(0))
    queries = [str(q).strip() for q in (data.get("queries") or []) if str(q).strip()]
    titles = [str(t).strip() for t in (data.get("titles") or []) if str(t).strip()]
    angle = str(data.get("angle") or "").strip()
    if not queries and not titles:
        raise RuntimeError(f"选题为空: {raw[:160]}")
    print(f"DeepSeek discover lane={lane and lane.get('id')} angle={angle!r} queries={queries} titles={titles}")
    return {"queries": queries[:6], "titles": titles[:3], "angle": angle}


def _fallback_queries(taste: dict, lane: dict | None = None) -> list[str]:
    if lane and lane.get("seeds"):
        return list(lane["seeds"])
    qs: list[str] = []
    for c in taste.get("prefer_categories") or []:
        t = c.get("title") or ""
        if t:
            qs.append(f"{t} 经典")
    for a in taste.get("prefer_authors") or []:
        n = a.get("name") or ""
        if n:
            qs.append(n)
    for b in taste.get("longest_reads") or []:
        author = (b.get("author") or "").strip()
        if author and author not in qs:
            qs.append(author)
    return qs[:6] or ["文学 经典", "翻译文学", "随笔 经典"]


def list_candidates_via_ai(
    taste: dict,
    recent_titles: list[str],
    on_shelf: set[str],
    seen: set[str],
    blocked_series: set[str],
    *,
    lane: dict | None = None,
    avoid_families: list[str] | None = None,
) -> list[dict]:
    """AI 提出检索 → 微信读书搜索落地 → 过滤后的候选池。"""
    try:
        plan = ai_discover_queries(
            taste, recent_titles, lane=lane, avoid_families=avoid_families
        )
        keywords = list(plan["titles"]) + list(plan["queries"])
    except Exception as exc:  # noqa: BLE001
        print(f"AI 选题失败，用画像兜底检索: {exc}")
        keywords = _fallback_queries(taste, lane)

    if lane and lane.get("seeds"):
        # 赛道种子靠前，提高命中差异化
        keywords = list(lane["seeds"]) + keywords

    out: list[dict] = []
    seen_bids: set[str] = set()
    avoid = set(avoid_families or [])
    want = (lane or {}).get("family")
    for kw in keywords:
        for book in search_ebooks(kw, count=12):
            bid = str(book.get("bookId") or "")
            if bid in seen_bids:
                continue
            if not _acceptable(book, on_shelf, seen, blocked_series):
                continue
            fam = book_family(book)
            if fam in avoid:
                continue
            if want and want != "other" and fam not in (want, "other"):
                # 历史赛道必须是 history；其他赛道拒绝 history
                if want == "history" and fam != "history":
                    continue
                if want != "history" and fam == "history":
                    continue
            seen_bids.add(bid)
            out.append(book)
            if len(out) >= 36:
                return out
    # 若赛道过滤过严，放宽：仅排除 avoid，不再强制 family
    if not out and want:
        print(f"lane={want} 严格过滤为空，放宽到仅避让 {avoid or '-'}")
        for kw in keywords:
            for book in search_ebooks(kw, count=12):
                bid = str(book.get("bookId") or "")
                if bid in seen_bids:
                    continue
                if not _acceptable(book, on_shelf, seen, blocked_series):
                    continue
                if book_family(book) in avoid:
                    continue
                if want != "history" and book_family(book) == "history":
                    continue
                seen_bids.add(bid)
                out.append(book)
                if len(out) >= 36:
                    return out
    return out


def taste_profile() -> dict:
    """从阅读统计抽喜好信号，交给 DeepSeek 选书（不是微信读书直接定稿）。"""
    profile: dict = {
        "prefer_categories": [],
        "prefer_authors": [],
        "longest_reads": [],
        "prefer_word": "",
    }
    try:
        data = weread("/readdata/detail", mode="overall")
    except Exception as exc:  # noqa: BLE001
        print(f"readdata fail: {exc}")
        return profile

    for c in data.get("preferCategory") or []:
        title = c.get("categoryTitle") or c.get("parentCategoryTitle") or ""
        if not title or title in BLOCKED_CATEGORIES:
            continue
        profile["prefer_categories"].append(
            {"title": title, "weight": c.get("val"), "books": c.get("readingCount")}
        )
        if len(profile["prefer_categories"]) >= 8:
            break
    for a in (data.get("preferAuthor") or [])[:6]:
        name = a.get("name") or ""
        if name:
            profile["prefer_authors"].append({"name": name, "count": a.get("count")})
    for row in data.get("readLongest") or []:
        book = row.get("book") or {}
        title = book.get("title") or (row.get("albumInfo") or {}).get("title") or ""
        if not title:
            continue
        fake = {"title": title, "category": book.get("category") or ""}
        if is_fast_or_non_reading(fake):
            continue
        profile["longest_reads"].append(
            {"title": title, "author": book.get("author") or "", "seconds": row.get("readTime")}
        )
        if len(profile["longest_reads"]) >= 8:
            break
    word = data.get("preferCategoryWord") or ""
    if any(b in word for b in BLOCKED_CATEGORIES):
        word = data.get("preferTimeWord") or ""
        if profile["prefer_categories"]:
            word = f"偏好阅读{profile['prefer_categories'][0]['title']}"
    profile["prefer_word"] = word or data.get("preferTimeWord") or ""
    return profile


def _candidate_card(book: dict) -> dict:
    return {
        "bookId": str(book.get("bookId") or ""),
        "title": book.get("title") or "",
        "author": book.get("author") or "",
        "category": (book.get("category") or "").split("-")[0],
        "intro": compress_intro(book.get("intro") or "", 120),
        "reason": (book.get("reason") or "")[:80],
        "rating": book.get("newRating"),
    }


def ai_pick_book(
    candidates: list[dict],
    taste: dict,
    recent_titles: list[str],
    *,
    lane: dict | None = None,
    avoid_families: list[str] | None = None,
) -> dict:
    """DeepSeek 按喜好从「AI 检索落地」候选中选一本；失败则退回列表第一本。"""
    if not candidates:
        raise RuntimeError("没有可用候选书（搜索与过滤后为空）")
    cards = [_candidate_card(b) for b in candidates]
    for c, b in zip(cards, candidates):
        c["family"] = book_family(b)
    by_id = {c["bookId"]: b for c, b in zip(cards, candidates)}
    lane_line = f"必须符合赛道「{(lane or {}).get('label') or '综合'}」。" if lane else ""
    avoid_line = (
        f"不要选题材族属于 {', '.join(avoid_families)} 的书。" if avoid_families else ""
    )
    system = (
        "你是书房荐书编辑。候选书来自你（或同级选题）提出的检索词在书城落地的结果，"
        "不是平台「为你推荐」。请按读者喜好只选一本正经可读之书。"
        f"{lane_line}{avoid_line}"
        "必须只输出 JSON：{\"bookId\":\"...\",\"why\":\"不超过40字\"}。"
        "贴合长读与偏好分类/作者，避免与近期已荐同质；"
        "期刊杂志、童书、快阅读不应入选；不要编造不在列表里的 bookId。"
        "why 只用常用汉字，避免生僻字。"
    )
    user = json.dumps(
        {
            "taste": taste,
            "recent_recommended_titles": recent_titles[-8:],
            "lane": lane,
            "avoid_families": avoid_families or [],
            "candidates": cards,
        },
        ensure_ascii=False,
    )
    raw = llm_chat(system, user, temperature=0.35)
    m = re.search(r"\{[\s\S]*\}", raw)
    if not m:
        print(f"DeepSeek 选书无 JSON，退回首本: {raw[:120]}")
        return candidates[0]
    try:
        picked = json.loads(m.group(0))
    except json.JSONDecodeError:
        print(f"DeepSeek 选书 JSON 解析失败，退回首本: {raw[:120]}")
        return candidates[0]
    bid = str(picked.get("bookId") or "")
    if bid not in by_id:
        print(f"DeepSeek 选了无效 bookId={bid!r}，退回首本")
        return candidates[0]
    print(f"DeepSeek pick {bid}: {picked.get('why', '')}")
    return by_id[bid]


def best_quote(book_id: str, intro: str) -> tuple[str, str]:
    try:
        data = weread("/book/bestbookmarks", bookId=book_id)
        items = data.get("items") or []
        items = sorted(items, key=lambda x: int(x.get("totalCount") or 0), reverse=True)
        for it in items:
            text = (it.get("markText") or "").strip()
            text = re.sub(r"\s+", " ", text)
            if efont_unsupported(text):
                continue
            if 16 <= len(text) <= 100:
                return text, "热门划线"
            if 10 <= len(text) <= 140:
                return text[:120], "热门划线"
    except Exception as exc:  # noqa: BLE001
        print(f"bestbookmarks fail: {exc}")
    parts = re.split(r"[。！？\n]", intro or "")
    for p in parts:
        p = p.strip().replace("\r", "")
        if 16 <= len(p) <= 72:
            return p + "。", "摘句"
    clean = re.sub(r"\s+", " ", (intro or "").strip())
    if clean:
        return clean[:96] + ("…" if len(clean) > 96 else ""), "摘句"
    return "好书不厌百回读。", "摘句"


def compress_intro(intro: str, limit: int = 220) -> str:
    """压成单段连贯中文：去掉换行/空格，避免屏上按字换行时把空格顶到行首像硬换行。"""
    text = (intro or "").replace("\r\n", "\n").replace("\r", "\n")
    text = text.replace("——", "――").replace("—", "――").replace("–", "-")
    text = re.sub(r"\s+", "", text).strip()
    if len(text) <= limit:
        return text
    cut = text[: limit - 1]
    for sep in ("。", "！", "？", "；", "，"):
        i = cut.rfind(sep)
        if i >= 60:
            return cut[: i + (1 if sep in "。！？" else 0)] + ("…" if sep not in "。！？" else "")
    return cut + "…"


def upgrade_cover(url: str) -> str:
    """尽量换成更大尺寸封面，便于屏上铺满。"""
    if not url:
        return url
    for a, b in (("t6_", "t7_"), ("/t6/", "/t7/"), ("s_", "t7_")):
        if a in url:
            return url.replace(a, b, 1)
    return url


def _strip_leading_punct(text: str) -> str:
    return (text or "").lstrip(_LEADING_PUNCT + " \t\n\r")


def _clip_recommend(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    cut = text[:limit]
    for sep in ("。", "！", "？", "；", "，"):
        i = cut.rfind(sep)
        if i >= limit // 2:
            return cut[: i + 1]
    return cut.rstrip("，、；") + "。"


def _normalize_recommend(text: str) -> str:
    text = re.sub(r"\s+", "", text or "")
    text = _strip_leading_punct(text)
    # efontCN 无 U+2014；中文破折号用两格 ――
    text = text.replace("——", "――").replace("—", "――").replace("–", "-")
    text = text.replace("「", "").replace("」", "").replace("“", "").replace("”", "")
    banned = ("适合你", "为你推荐", "根据你的", "AI推荐", "人工智能")
    for w in banned:
        text = text.replace(w, "")
    return _strip_leading_punct(text)


def write_recommend_template(title: str, author: str, category: str, intro: str) -> str:
    """模板兜底；字数 ∈ [3.5×35, 4×35]。"""
    cat = (category or "读物").split("-")[0]
    who = (author or "作者").strip()
    who_s = who if len(who) <= 6 else who[:6]
    name = re.sub(r"[（(][^）)]*[）)]", "", title or "这本书").strip() or (title or "这本书")
    if len(name) > 14:
        name = name[:13] + "…"

    parts = [p.strip() for p in re.split(r"[。！？\n]", intro or "") if len(p.strip()) >= 8]
    hooks = [_strip_leading_punct(p) for p in parts[:3]]
    hooks = [h for h in hooks if h]
    if not hooks:
        hooks = ["把书里真正要紧的话读明白"]
    for i, h in enumerate(hooks):
        if len(h) > 40:
            hooks[i] = h[:39]
        if not hooks[i].endswith(("。", "！", "？")):
            hooks[i] += "。"

    text = (
        f"《{name}》是{cat}向作品，{who_s}写来清楚不绕弯。"
        f"{hooks[0]}"
        f"读它不只记概念，更是把日常里含糊的判断理顺。"
        f"想放一本随时可翻的书在手边，这一本值得。"
    )
    extra_i = 1
    fill_i = 0
    while len(re.sub(r"\s+", "", text)) < RECOMMEND_MIN_CHARS:
        if extra_i < len(hooks):
            text += hooks[extra_i]
            extra_i += 1
        elif fill_i < len(_RECOMMEND_FILLERS):
            text += _RECOMMEND_FILLERS[fill_i]
            fill_i += 1
        else:
            text += "好书耐读，慢慢品就够了。"
            break

    text = _normalize_recommend(text)
    text = _clip_recommend(text, RECOMMEND_MAX_CHARS)
    if len(text) < RECOMMEND_MIN_CHARS:
        text = _strip_leading_punct(text + _RECOMMEND_FILLERS[0])
        text = _clip_recommend(text, RECOMMEND_MAX_CHARS)
    return _strip_leading_punct(text)


def ai_write_recommend(title: str, author: str, category: str, intro: str, quote: str) -> str:
    """DeepSeek 写荐读：谈书本身，122–140 字；失败退回模板。"""
    system = (
        "你是中文荐书文案编辑。写一段连贯荐读正文，只谈这本书本身的内容与读感。"
        f"纯中文，不要换行，不要引号，字数严格在 {RECOMMEND_MIN_CHARS} 到 {RECOMMEND_MAX_CHARS} 个汉字/字符之间。"
        "禁止出现：适合你、为你推荐、根据你的、AI、读者画像、个性化。"
        "只用常用汉字与常规标点，严禁生僻字（屏上点阵字库会缺字显示方框）。"
        "不要以标点开头。只输出正文，不要标题或解释。"
        "可用中文破折号写成两个连续字符――。"
    )
    user = json.dumps(
        {
            "title": title,
            "author": author,
            "category": category,
            "intro": compress_intro(intro, 280),
            "quote": quote,
        },
        ensure_ascii=False,
    )
    try:
        raw = llm_chat(system, user, temperature=0.55)
    except Exception as exc:  # noqa: BLE001
        print(f"DeepSeek 荐读失败，用模板: {exc}")
        return write_recommend_template(title, author, category, intro)

    text = _normalize_recommend(raw)
    fill_i = 0
    while len(text) < RECOMMEND_MIN_CHARS and fill_i < len(_RECOMMEND_FILLERS):
        text = _normalize_recommend(text + _RECOMMEND_FILLERS[fill_i])
        fill_i += 1
    text = _clip_recommend(text, RECOMMEND_MAX_CHARS)
    if len(text) < RECOMMEND_MIN_CHARS:
        print(f"DeepSeek 荐读字数不合规({len(text)})，用模板")
        return write_recommend_template(title, author, category, intro)
    return text


def _safe_substr(hay: str, needle: str, lo: int = 2, hi: int = 16) -> str:
    needle = scrub_efont(_normalize_recommend(needle or ""))
    if not needle or needle not in (hay or ""):
        return ""
    if len(needle) < lo or len(needle) > hi:
        return ""
    return needle


def _split_recommend(rec: str) -> tuple[str, str]:
    text = _normalize_recommend(rec)
    for sep in ("？", "。", "！"):
        i = text.find(sep)
        if 8 <= i <= 48:
            return text[: i + 1], text[i + 1 :]
    mid = min(36, max(12, len(text) // 3))
    return text[:mid], text[mid:]


def _fallback_eye(quote: str, title: str) -> str:
    q = scrub_efont(_normalize_recommend(quote))
    for n in (4, 5, 6, 3):
        if len(q) >= n:
            eye = q[:n]
            if efont_safe(eye):
                return eye
    t = scrub_efont(re.sub(r"[（(].*?[）)]", "", title or ""))
    return t[:4] if t else "荐读"


def build_editorial(
    title: str, author: str, category: str, intro: str, quote: str, recommend: str
) -> dict:
    """题眼/为何/句内高亮/荐读分段。优先 AI JSON，失败用规则兜底。"""
    blurb = scrub_efont(compress_intro(intro, 220))
    rec = scrub_efont(_normalize_recommend(recommend))
    cat = (category or "图书").split("-")[0]
    system = (
        "你为横屏荐书板写结构化短文案。只输出一个 JSON 对象，不要 markdown。"
        "字段：why(8-14字今日为何), eye(4-6字题眼), lede(16-26字导语),"
        "quote_hit(必须是 quote 连续子串4-10字), intro_hit(必须是 intro 连续子串8-20字),"
        "voice(只能是：语气·一问 / 语气·物性 / 语气·对照 之一),"
        "recommend_lead(荐读首句,问句优先,≤36字), recommend_body(其余荐读),"
        "recommend_hit(必须是 recommend_body 连续子串6-16字)。"
        "只用常用汉字，禁止生僻字与英文引号。破折号用――。"
    )
    user = json.dumps(
        {
            "title": title,
            "author": author,
            "category": cat,
            "quote": quote,
            "intro": blurb,
            "recommend": rec,
        },
        ensure_ascii=False,
    )
    data: dict = {}
    try:
        raw = llm_chat(system, user, temperature=0.4)
        m = re.search(r"\{[\s\S]*\}", raw)
        if m:
            data = json.loads(m.group(0))
    except Exception as exc:  # noqa: BLE001
        print(f"editorial AI fail: {exc}")
        data = {}

    lead, body = _split_recommend(rec)
    eye = scrub_efont(str(data.get("eye") or ""))[:6]
    if not eye or not efont_safe(eye):
        eye = _fallback_eye(quote, title)
    why = scrub_efont(_normalize_recommend(str(data.get("why") or "")))[:14]
    if len(why) < 4:
        why = f"今日{cat}"[:14]
    lede = scrub_efont(_normalize_recommend(str(data.get("lede") or "")))[:28]
    if len(lede) < 8:
        lede = scrub_efont(compress_intro(blurb, 26))
    voice = str(data.get("voice") or "语气·一问")
    if voice not in ("语气·一问", "语气·物性", "语气·对照"):
        voice = "语气·一问"
    voice = voice.replace("·", " · ")

    q_hit = _safe_substr(quote, str(data.get("quote_hit") or ""), 3, 12)
    if not q_hit and eye in (quote or ""):
        q_hit = eye
    i_hit = _safe_substr(blurb, str(data.get("intro_hit") or ""), 6, 24)
    if not i_hit and len(blurb) >= 12:
        i_hit = blurb[-18:] if len(blurb) >= 18 else blurb

    ai_lead = scrub_efont(_normalize_recommend(str(data.get("recommend_lead") or "")))
    ai_body = scrub_efont(_normalize_recommend(str(data.get("recommend_body") or "")))
    if 6 <= len(ai_lead) <= 40 and len(ai_body) >= 20:
        lead, body = ai_lead, ai_body
    r_hit = _safe_substr(body, str(data.get("recommend_hit") or ""), 4, 18)

    # 荐读总长仍受原区间约束：过长裁 body
    total = lead + body
    if len(total) > RECOMMEND_MAX_CHARS + 20:
        body = _clip_recommend(body, max(40, RECOMMEND_MAX_CHARS + 20 - len(lead)))
        r_hit = _safe_substr(body, r_hit, 4, 18)

    return {
        "why": why,
        "eye": eye,
        "lede": lede,
        "quote_hit": q_hit,
        "intro_hit": i_hit,
        "voice": voice,
        "recommend_lead": lead,
        "recommend": body,
        "recommend_hit": r_hit,
    }


def rating_fields(book: dict) -> tuple[str, str]:
    rating = book.get("newRating")
    count = book.get("newRatingCount")
    detail = book.get("newRatingDetail") or {}
    if rating is None and isinstance(detail, dict):
        title = detail.get("title") or ""
        return (title or "—"), ""
    if rating is None:
        return "—", ""
    try:
        score = f"{float(rating) / 10:.1f}" if float(rating) > 10 else f"{float(rating):.1f}"
    except (TypeError, ValueError):
        score = str(rating)
    people = ""
    if count:
        try:
            n = int(count)
            people = f"{n // 10000} 万人评" if n >= 10000 else f"{n} 人评"
        except (TypeError, ValueError):
            people = ""
    return score, people


def enrich(book: dict) -> dict:
    bid = str(book["bookId"])
    info = {}
    try:
        info = weread("/book/info", bookId=bid)
    except Exception as exc:  # noqa: BLE001
        print(f"book/info fail: {exc}")
    title = info.get("title") or book.get("title") or ""
    author = info.get("author") or book.get("author") or ""
    cover = upgrade_cover(info.get("cover") or book.get("cover") or "")
    intro = info.get("intro") or book.get("intro") or ""
    category = info.get("category") or book.get("category") or ""
    word_count = info.get("wordCount")
    words = ""
    if word_count:
        try:
            w = int(word_count)
            words = f"约 {w // 10000} 万字" if w >= 10000 else f"约 {w} 字"
        except (TypeError, ValueError):
            words = ""
    merged = {**book, **{k: v for k, v in info.items() if v not in (None, "")}}
    score, people = rating_fields(merged)
    quote, quote_src = best_quote(bid, intro)
    # 划线/简介/荐读去掉字库没有的字，避免方框
    if efont_unsupported(quote):
        quote = scrub_efont(quote)
        if len(quote) < 10:
            quote, quote_src = scrub_efont(compress_intro(intro, 96)), "摘句"
            if len(quote) < 8:
                quote, quote_src = "好书不厌百回读。", "摘句"
    blurb = scrub_efont(compress_intro(intro, 220))
    rec = ai_write_recommend(title, author, category, intro, quote)
    if efont_unsupported(rec):
        rec = scrub_efont(rec)
    if len(re.sub(r"\s+", "", rec)) < RECOMMEND_MIN_CHARS:
        rec = write_recommend_template(title, author, category, intro)
        rec = scrub_efont(rec)
    editorial = build_editorial(title, author, category, blurb, quote, rec)
    return {
        "updated_at": now_str(),
        "slot": "",
        "why": editorial["why"],
        "bookId": bid,
        "title": title,
        "author": author,
        "cover": cover,
        "category": (category or "").split("-")[0] or "图书",
        "rating": score,
        "rating_count": people,
        "words": words,
        "quote": quote,
        "quote_hit": editorial["quote_hit"],
        "quote_source": quote_src,
        "eye": editorial["eye"],
        "lede": editorial["lede"],
        "intro": blurb,
        "intro_hit": editorial["intro_hit"],
        "voice": editorial["voice"],
        "recommend_lead": editorial["recommend_lead"],
        "recommend": editorial["recommend"],
        "recommend_hit": editorial["recommend_hit"],
    }


def slot_label(index: int, total: int = PLAYLIST_SIZE) -> str:
    return f"今日荐读 · 第 {index}/{total} 本"


def build_document(
    exclude_ids: set[str] | None = None,
    *,
    lane: dict | None = None,
    avoid_families: list[str] | None = None,
) -> dict:
    """生成一本（供片单循环调用）。"""
    on_shelf = shelf_ids()
    hist = load_history()
    seen = recent_ids(hist) | (exclude_ids or set())
    blocked = recent_series(hist)
    taste = taste_profile()
    recent_titles = [str(e.get("title") or "") for e in hist if e.get("title")]
    print(
        f"shelf={len(on_shelf)} recent={len(seen)} series_block={blocked or '-'} "
        f"lane={(lane or {}).get('id') or '-'} avoid={avoid_families or '-'} "
        f"taste={taste.get('prefer_word') or '-'} model={LLM_MODEL}"
    )
    cands = list_candidates_via_ai(
        taste,
        recent_titles,
        on_shelf,
        seen,
        blocked,
        lane=lane,
        avoid_families=avoid_families,
    )
    if exclude_ids:
        cands = [c for c in cands if str(c.get("bookId") or "") not in exclude_ids]
    print(f"ai-search candidates={len(cands)} llm={llm_chat_url()}")
    cand = ai_pick_book(
        cands, taste, recent_titles, lane=lane, avoid_families=avoid_families
    )
    doc = enrich(cand)
    doc["family"] = book_family(doc)
    doc["lane"] = (lane or {}).get("id") or ""
    return doc


def build_day_playlist(size: int = PLAYLIST_SIZE) -> dict:
    """每天一次：按五条差异化赛道选出 size 本（历史最多 1）。"""
    books: list[dict] = []
    picked: set[str] = set()
    used_families: list[str] = []
    lanes = list(PLAYLIST_LANES[:size])
    # 打乱非历史赛道顺序，历史轨固定为最后一档之一，避免前几本全挤进历史
    hist_lanes = [ln for ln in lanes if ln["family"] == "history"]
    other_lanes = [ln for ln in lanes if ln["family"] != "history"]
    random.shuffle(other_lanes)
    ordered = other_lanes + hist_lanes
    for i, lane in enumerate(ordered):
        print(f"--- playlist {i + 1}/{size} lane={lane['id']} ({lane['label']}) ---")
        # 硬约束：非历史赛道一律避开 history；历史赛道整天只有一档
        # 不再把已用 fiction/essay 等列入硬避让，否则「中文小说+翻译文学」会自相残杀
        avoid: list[str] = []
        if lane["family"] != "history":
            avoid.append("history")
        doc = build_document(exclude_ids=picked, lane=lane, avoid_families=avoid)
        bid = str(doc.get("bookId") or "")
        fam = doc.get("family") or book_family(doc)
        if not bid or bid in picked:
            raise RuntimeError(f"片单第 {i + 1} 本无效或重复")
        if lane["family"] != "history" and fam == "history":
            print(f"warn: lane={lane['id']} 仍落到历史，重试一次…")
            doc = build_document(
                exclude_ids=picked | {bid},
                lane=lane,
                avoid_families=list(dict.fromkeys(avoid + ["history"])),
            )
            bid = str(doc.get("bookId") or "")
            fam = doc.get("family") or book_family(doc)
        if fam == "history" and "history" in used_families:
            raise RuntimeError("片单历史向超过 1 本，中止")
        doc["slot"] = slot_label(i + 1, size)
        doc["family"] = fam
        books.append(doc)
        picked.add(bid)
        if fam not in used_families:
            used_families.append(fam)
        print(f"picked family={fam} title={doc.get('title')}")
    today = now_bj().strftime("%Y-%m-%d")
    return {
        "date": today,
        "updated_at": now_str(),
        "count": len(books),
        "books": books,
        "diversity": used_families,
    }


def write_day(day: dict) -> None:
    _atomic_write_json(DAY_OUT, day)
    _atomic_write_json(DOCS_DAY, day)
    # 兼容旧单本 URL：第一本
    first = (day.get("books") or [None])[0]
    if isinstance(first, dict):
        write_doc(first, append_history=False)
    hist = load_history()
    for b in day.get("books") or []:
        hist.append(
            {
                "at": day.get("updated_at"),
                "bookId": b.get("bookId"),
                "title": b.get("title"),
                "slot": b.get("slot"),
                "date": day.get("date"),
            }
        )
    save_history(hist)
    titles = " / ".join(str(b.get("title") or "") for b in day.get("books") or [])
    print(f"wrote {DAY_OUT} + {DOCS_DAY} date={day.get('date')} count={day.get('count')} << {titles}")


def write_doc(doc: dict, append_history: bool = True) -> None:
    _atomic_write_json(OUT, doc)
    _atomic_write_json(DOCS_BOOK, doc)
    if append_history:
        hist = load_history()
        hist.append(
            {
                "at": doc["updated_at"],
                "bookId": doc["bookId"],
                "title": doc["title"],
                "slot": doc.get("slot"),
            }
        )
        save_history(hist)
    print(f"wrote {OUT} + {DOCS_BOOK} << {doc['title']} / {doc['bookId']}")


def _json_response(handler: http.server.SimpleHTTPRequestHandler, path: Path) -> None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        handler.send_error(500, f"read json failed: {exc}")
        return
    data["server_now"] = int(time.time())
    body = json.dumps(data, ensure_ascii=False).encode("utf-8")
    handler.send_response(200)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Cache-Control", "no-store")
    handler.end_headers()
    handler.wfile.write(body)


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(OUT_DIR), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))

    def do_GET(self):  # noqa: N802
        if self.path in ("/", "/weread_day.json", "/weread_day.json/"):
            _json_response(self, DAY_OUT if DAY_OUT.is_file() else OUT)
            return
        if self.path in ("/weread_book.json", "/weread_book.json/"):
            _json_response(self, OUT)
            return
        return super().do_GET()


def day_playlist_ready_today() -> bool:
    if not DAY_OUT.is_file():
        return False
    try:
        data = json.loads(DAY_OUT.read_text(encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return False
    today = now_bj().strftime("%Y-%m-%d")
    books = data.get("books") or []
    return data.get("date") == today and len(books) >= PLAYLIST_SIZE


def run_scheduler(httpd: socketserver.TCPServer) -> None:
    """每天 7 点生成 5 本片单；失败则在窗口内重试。白天只提供 HTTP。"""
    done_date = ""
    last_attempt = 0.0
    if day_playlist_ready_today():
        done_date = now_bj().strftime("%Y-%m-%d")
        print(f"today playlist already ready ({done_date})")
    while True:
        now = now_bj()
        today = now.strftime("%Y-%m-%d")
        in_window = now.hour == DAILY_HOUR and now.minute < RETRY_WINDOW_MIN
        # 7 点后若仍无今日片单（开机晚了），允许补一次
        missed = (
            done_date != today
            and not day_playlist_ready_today()
            and now.hour >= DAILY_HOUR
        )
        need = done_date != today and (in_window or missed)
        if need and time.monotonic() - last_attempt >= RETRY_GAP_SEC:
            last_attempt = time.monotonic()
            try:
                print(f"daily {DAILY_HOUR}:00 playlist generating…")
                day = build_day_playlist(PLAYLIST_SIZE)
                write_day(day)
                done_date = today
            except Exception as exc:  # noqa: BLE001
                print(f"daily playlist failed: {exc}")
        httpd.handle_request()


def main() -> None:
    parser = argparse.ArgumentParser(description="微信读书荐书板旁路")
    parser.add_argument("--once", action="store_true", help="立即生成今日 5 本片单后退出")
    parser.add_argument("--serve", action="store_true", help="挂 HTTP；每天 7 点更新片单")
    args = parser.parse_args()

    if args.once or not args.serve:
        day = build_day_playlist(PLAYLIST_SIZE)
        write_day(day)
        if not args.serve:
            print(f"本机预览: http://127.0.0.1:{PORT}/weread_day.json （需 --serve）")
            return

    if not day_playlist_ready_today():
        try:
            write_day(build_day_playlist(PLAYLIST_SIZE))
        except Exception as exc:  # noqa: BLE001
            print(f"初始片单生成失败，仍启动服务: {exc}")

    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("0.0.0.0", PORT), Handler)
    httpd.timeout = 0.5
    print(f"serving {OUT_DIR} on 0.0.0.0:{PORT}")
    print(f"Tab5 URL: http://<Mac局域网IP>:{PORT}/weread_day.json")
    print(
        f"daily={DAILY_HOUR}:00 size={PLAYLIST_SIZE} "
        f"retry_window={RETRY_WINDOW_MIN}m model={LLM_MODEL} llm={llm_chat_url()}"
    )
    try:
        run_scheduler(httpd)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
