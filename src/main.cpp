/*
 * fp -- 第一人称视角（Direct3D 11）
 *
 * 现在有这几件事：
 *   1) 第一人称：低头能看见自己的身子（胸、两条胳膊垂在身体两侧、两条腿），
 *      跳起来整个人一起离地、膝盖收起来；手上不挡视线的时候看不见手，正常；
 *   2) 地图无限：玩家周围一圈的地面、草、花、树是一个"窗口"，
 *      走到哪生成到哪（分帧攒、双缓冲换），走多远都不会看到地图的边；
 *   3) 花和树是装饰，树干有碰撞箱，撞上去会被推开，穿不过去；
 *   4) 天和太阳都在着色器里画：天顶深蓝、地平线发白，太阳是亮盘 + 三层光晕；
 *   5) 河是一条真的凹下去的河道：水面比岸低、半透明能看见河床，还有反光和碎光，
 *      越靠河石子越多。
 *
 * 世界里的东西全是"世界坐标的函数"（格子坐标哈希 → 草/花/树的位置），
 * 所以重新攒一遍的时候东西还在原地，不会长腿跑；
 * 树的位置和碰撞箱用的是同一个函数，永远对得上。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

using namespace DirectX;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define WIN_W 960
#define WIN_H 600

/* ---------------- 顶点 ---------------- */
struct Vtx {
    XMFLOAT3 pos;
    XMFLOAT3 nrm;
    XMFLOAT3 col;
    XMFLOAT2 aux;       /* x = 呼吸权重，y = 头发摆动权重（0 = 完全不动） */
};

/* 攒顶点时临时用的权重：摆好再 Push，Push 完就归零 */
static XMFLOAT2 g_aux = { 0.0f, 0.0f };
static int      g_tube_caps = 1;   /* 圆管要不要端盖：叠球的地方要关掉 */

/* 躯干上的两个乳头：它们不是另外的几何体，而是把躯干表面按距离往外推出来的一块，
 * 所以和身体是同一张曲面，天生连在一起，不会有交叠和锯齿。
 * 影响半径特意压到 0.115 米（比乳间距的一半还小），中间那条缝就不会被顶起来，
 * 看上去就是清清楚楚两个，而不是糊成一坨 / 变成四个。 */
static const struct {
    XMFLOAT3 c;        /* 乳头中心（局部坐标） */
    XMFLOAT3 dir;      /* 往哪个方向鼓 */
    float    amp;      /* 鼓多高 */
    float    R;        /* 影响范围 */
} g_bulge[2] = {
    { {  0.082f, 1.242f, 0.062f }, {  0.52f,  0.10f,  0.85f }, 0.090f, 0.128f },
    { { -0.082f, 1.242f, 0.062f }, { -0.52f,  0.10f,  0.85f }, 0.090f, 0.128f }
};

/* ---------------- 全局 ---------------- */
static wchar_t g_errmsg[1024];        /* 初始化失败时显示具体原因 */
static ID3D11Device           *g_dev;
static ID3D11DeviceContext    *g_ctx;
static IDXGISwapChain         *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11DepthStencilView *g_dsv;
static ID3D11VertexShader     *g_vs;
static ID3D11PixelShader      *g_ps;
static ID3D11VertexShader     *g_sky_vs;      /* 天空那一遍用的 */
static ID3D11PixelShader      *g_sky_ps;
static ID3D11InputLayout      *g_sky_layout;
static ID3D11Buffer           *g_sky_vb;
static ID3D11InputLayout      *g_layout;
static ID3D11Buffer           *g_cb;
static ID3D11RasterizerState  *g_rs;
static ID3D11DepthStencilState *g_ds_on, *g_ds_off, *g_ds_nowrite;
static ID3D11BlendState       *g_blend;

struct CBData {
    XMMATRIX mvp;
    XMFLOAT4 light;         /* xyz = 光来的方向，w = 环境光强度（背光面有多亮） */
    XMFLOAT4 anim;          /* x = 时间，y = 雾的起点，z = 雾的终点 */
    XMFLOAT4 eye;           /* xyz = 镜头在世界里的位置（算雾用） */
};
static CBData g_cbd;

/* 世界（地面 + 草 + 花 + 树）：双缓冲，一面画着，一面在攒 */
static ID3D11Buffer *g_world_vb[2];
static UINT          g_world_n[2];
static UINT          g_world_cap[2];
static int           g_world_cur;    /* 现在画的是哪一面 */
static ID3D11Buffer *g_hand_vb;      /* 动态：右上角的帧率数字（跟着镜头） */
static ID3D11Buffer *g_body_vb;      /* 动态：自己这副身子（世界空间，跟着人走） */
static UINT          g_body_n;
static ID3D11Buffer *g_woman_vb;     /* 动态：那个女性角色（弯腰时要重建） */
static UINT          g_woman_n;
static float         g_bend_t;       /* 弯腰动作还剩多久 */
static float         g_rmb;          /* 右键被按下 */
static float         g_woman_yaw;
static int           g_woman_uploaded;
static UINT          g_hand_n;
static XMFLOAT3      g_sky_ray[3];      /* 天空三角形三个角看出去的方向点 */
static XMFLOAT3      g_sky_eye;         /* 算天空光线用的镜头位置 */

static int g_dbg_handsonly = 0;

/* 她的姿势：0 = 站在地上，1 = 坐在地上，2 = 仰面躺下、两腿弯起来 */
#define POSE_STAND 0
#define POSE_SIT   1
#define POSE_LIE   2

/* ---------------- 调试用：命令行参数 ----------------
 * fp.exe --yaw 1.2 --pitch 0.1      固定视角
 * fp.exe --look                     镜头直接对准她（用来确认她到底画出来没有）
 * fp.exe --dist 3                   她站在正前方多少米（默认 5~9 米随机）
 * fp.exe --bend 1                   让她保持弯腰
 * fp.exe --jy 1.2                   把自己"跳起来"的高度定死（截图看身体用）
 * fp.exe --self 3                   从外面 3 米处看自己这副身子（看姿势对不对）
 * fp.exe --seed 7                   地图（那条河）用固定种子，前后截图能对上
 * fp.exe --px 300 --pz 300          开局站在世界坐标 (300,300)（测无限地图）
 * fp.exe --frames 120               先空跑 120 帧再截图（看世界重攒完没有）
 * fp.exe --shot out.tga             跑一帧、写进文件、退出（不用人看屏幕）
 * fp.exe --log out.txt              把世界顶点数/她的位置/镜头朝向写下来
 *
 * 注意：这里不能用 __argc/__argv —— 这个程序是按 GUI 子系统链接的，
 * 启动代码没跑 _setargv，__argv 是 NULL，解引用会直接 0xc0000005。
 * 所以参数从 GetCommandLineW 自己拆。 */
static float g_dbg_yaw   = 0.0f;
static float g_dbg_pitch = 0.0f;
static int   g_dbg_set_look = 0, g_dbg_set_yaw = 0, g_dbg_set_pitch = 0;
static int   g_dbg_bend = 0;
static int   g_dbg_pose = -1;      /* --pose stand|sit */
static float g_dbg_dist = 0.0f;
static float g_dbg_wz = 999.0f;    /* 直接指定她站在 z 多少 */
static float g_dbg_wyaw = 999.0f;  /* 直接指定她朝哪边转 */
static float g_dbg_jy = -1.0f;     /* --jy：把跳起来的高度定死（截图对比用） */static float g_dbg_self = 0.0f;    /* --self 3：从外面 3 米处看自己这副身子（调试） */
static float g_dbg_px = 0.0f, g_dbg_pz = 0.0f;   /* --px/--pz：把人放到世界坐标某处 */
static int   g_dbg_frames = 1;     /* --frames 60：先空跑 60 帧再截图（看世界重攒完没有） */
static int   g_dbg_walk = 0;       /* --walk 600：自动往前走 600 帧（测边走边重攒世界） */
static double g_perf_sum = 0.0;    /* Render 的累计耗时 / 帧数（--log 里报个平均） */
static int    g_perf_n = 0;
static int   g_dbg_seed = -1;      /* --seed 123：地图（河）用固定的随机种子 */
static char  g_dbg_shot[260];
static char  g_dbg_log[260];
static int   g_dbg_shot_on = 0, g_dbg_log_on = 0;
static float g_fixed_dt = 1.0f / 60.0f;

#define DBG_MAXARG 32
static char  g_args[DBG_MAXARG][260];
static int   g_nargs;

/* 把命令行拆成一个个参数（跳过 exe 自己的路径） */
static void SplitCmdLine(void)
{
    const wchar_t *p = GetCommandLineW();
    int i = 0;

    g_nargs = 0;
    while (*p && g_nargs < DBG_MAXARG) {
        wchar_t wbuf[260];
        int n = 0;

        while (*p == L' ' || *p == L'\t')
            p++;
        if (!*p)
            break;
        if (*p == L'"') {                    /* 带引号的参数 */
            p++;
            while (*p && *p != L'"' && n < 259)
                wbuf[n++] = *p++;
            if (*p == L'"')
                p++;
        } else {
            while (*p && *p != L' ' && *p != L'\t' && n < 259)
                wbuf[n++] = *p++;
        }
        wbuf[n] = 0;
        if (i > 0)                           /* i == 0 是可执行文件本身 */
            WideCharToMultiByte(CP_ACP, 0, wbuf, -1, g_args[g_nargs], 260, NULL, NULL), g_nargs++;
        i++;
    }
}

/* 找 "--key 值"，找到返回 1 并把值写进 out */
static int DbgArgF(const char *key, float *out)
{
    int i;

    for (i = 0; i + 1 < g_nargs; i++) {
        if (lstrcmpiA(g_args[i], key) == 0) {
            *out = (float)atof(g_args[i + 1]);
            return 1;
        }
    }
    return 0;
}

static int DbgArgS(const char *key, char *out, int n)
{
    int i;

    for (i = 0; i + 1 < g_nargs; i++) {
        if (lstrcmpiA(g_args[i], key) == 0) {
            lstrcpynA(out, g_args[i + 1], n);
            return 1;
        }
    }
    return 0;
}

static void ParseDbgArgs(void)
{
    float f = 0.0f;
    char  sbuf[64];
    int   i;

    SplitCmdLine();
    if (DbgArgF("--bend", &f))
        g_dbg_bend = (f > 0.5f);
    if (DbgArgS("--pose", sbuf, sizeof(sbuf))) {
        g_dbg_pose = POSE_LIE;
        if (lstrcmpiA(sbuf, "stand") == 0)
            g_dbg_pose = POSE_STAND;
        else if (lstrcmpiA(sbuf, "sit") == 0)
            g_dbg_pose = POSE_SIT;
    }
    g_dbg_set_yaw   = DbgArgF("--yaw", &g_dbg_yaw);
    g_dbg_set_pitch = DbgArgF("--pitch", &g_dbg_pitch);
    DbgArgF("--dist", &g_dbg_dist);
    DbgArgF("--wz", &g_dbg_wz);
    DbgArgF("--wyaw", &g_dbg_wyaw);
    DbgArgF("--jy", &g_dbg_jy);
    DbgArgF("--self", &g_dbg_self);
    DbgArgF("--px", &g_dbg_px);
    DbgArgF("--pz", &g_dbg_pz);
    {
        float fr = 1.0f;
        float wk = 0.0f;

        if (DbgArgF("--frames", &fr))
            g_dbg_frames = (int)fr;
        if (DbgArgF("--walk", &wk))
            g_dbg_walk = (int)wk;
    }
    {
        float sd = -1.0f;

        if (DbgArgF("--seed", &sd))
            g_dbg_seed = (int)sd;
    }
    if (DbgArgS("--shot", g_dbg_shot, sizeof(g_dbg_shot)))
        g_dbg_shot_on = 1;
    if (DbgArgS("--log", g_dbg_log, sizeof(g_dbg_log)))
        g_dbg_log_on = 1;
    for (i = 0; i < g_nargs; i++)
        if (lstrcmpiA(g_args[i], "--look") == 0)
            g_dbg_set_look = 1;
}

static float g_yaw, g_pitch;      /* 朝向：左右 / 上下 */
static double g_clock;            /* 从进游戏到现在几秒（呼吸、摆腿都用它） */
static double g_fps_now = 0.0;    /* 平滑后的帧率，显示在右上角 */
static float  g_walk;             /* 走路的相位：走着才涨，停下慢慢回到 0 */
static float  g_walk_amp;         /* 走的幅度：0 = 站着，1 = 正常走 */
static float  g_jy, g_jvy;        /* 跳起来的高度和速度 */
static float  g_ground_y;         /* 脚下地面的高度（河滩是往下斜的） */
/* 自己这副身子整体抬高多少：脚下地面 + 跳起来的高度。
 * 躯干、胳膊、腿都加这一个数，所以人是站在地上、不是浮在地上。 */
static float SelfLift(void) { return g_ground_y + g_jy; }
static int    g_first_frame = 1;  /* 第一帧把光标拉回正中 */
static float  g_dt = 1.0f / 60.0f; /* 真实帧时间（移动、动画都按它算） */

/* 地图上随机生成的河 */
static int   g_river_on;
static int   g_river_dir;        /* 0 = 河沿 X 方向流，1 = 沿 Z */
static float g_river_w, g_river_c, g_river_amp, g_river_freq, g_river_ph;

/* 那个女性角色站在哪儿（碰撞用） */
static XMFLOAT3 g_woman_at;
static int      g_woman_live;
static float    g_bend_amt;      /* 0 = 站着，1 = 弯到最深（腿岔开、上身转下去都看它） */
static float    g_lie_amt = 1.0f;  /* 0 = standing, 1 = lying flat (default: lying) */
static float    g_sit_lift;      /* 坐姿整体抬高多少（见 SitGroundFix） */

static int      g_pose = POSE_LIE;
static float    g_pose_down = 0.20f;    /* 坐姿低头多少（弧度） */

/* ---- 躺姿 ----
 * 躺下这件事就是"把她整个人绕 X 轴转 90 度"：头顶转向前方（+Z）、
 * 脸朝上、脚也朝前抬起来。所以所有关节还是用站姿的局部坐标写，
 * 由 Xform() 统一转过去，不用另算一套。 */
#define LIE_ROT   1.5707963f            /* 躺下 = 转 90 度 */
#define LIE_LIFT  0.015f                /* 后脑贴着地：背在局部 y=0，转过来会入地 0.097，抬回来 */
#define LIE_KNEE  0.480f                /* 膝盖在局部的高度：腿弯起来，落在世界 y≈0.38 */
#define LIE_FOOT  0.080f                /* 脚踝：踩回地上（世界 y≈0.06） */

/* ---- 坐姿的全部数字都在这里，照着骨骼长度算出来的，不要随手改 ----
 * 站立时的关节高度：胯 0.925、肩 1.345、脖子根 1.455、头心 1.60、身高 1.672。 */
#define SIT_HIP    0.115f               /* 坐下来胯中心的高度 */
#define SIT_FOLD   1.325f               /* 肩膀以下才开始往一起叠（再往上要刚性平移） */
#define SIT_KMIN   0.10f                /* 胯那一带压到站姿的多少 */
#define SIT_HEAD   0.86f                /* 坐下来头顶离地大概多高 */
#define WOMAN_R   0.30f          /* 她的碰撞半径 */
#define PLAYER_R  0.34f          /* 你的碰撞半径 */

/* 你自己这副身子的基准尺寸：眼睛多高、胯多高。
 * 低头看见的躯干和腿都挂在这两个数上，跳起来再整体加 g_jy。 */
#define SELF_EYE  1.70f          /* 站着的时候眼睛离地多高 */
#define SELF_HIP  0.92f          /* 站着的时候胯中心离地多高 */
static XMFLOAT3 g_eye = { 0, 1.7f, 0 };
static XMMATRIX g_proj;
static XMMATRIX g_view;
static XMMATRIX g_hand_mvp;

/* ---------------- 攒顶点 ----------------
 * 有两块数组，两个计数器：
 *   g_vbuf_a / g_vn  —— 自己这副身子、她、右上角的帧率，每帧从头攒一遍；
 *   g_wbuf / g_wn  —— 世界（地面/草/花/树/太阳），分好几帧才攒得完。
 * 两块必须分开：世界的活儿是跨帧的，要是和每帧那点顶点共用一个计数器，
 * 中间插进来的"自己/她/UI"就会把世界的顶点冲掉一半（地面会缺一大块）。 */
#define MAX_VTX       (760000)
#define MAX_WORLD_VTX (620000)
static Vtx   g_vbuf_a[MAX_VTX];
static Vtx   g_wbuf[MAX_WORLD_VTX];
static UINT  g_vn;                   /* 每帧那块攒到第几个 */
static UINT  g_wn;                   /* 世界攒到第几个 */
static Vtx  *g_pt_dst = g_vbuf_a;    /* Push 现在往哪块里攒 */
static UINT *g_pt_n   = &g_vn;       /* 那个块的计数器 */
static UINT  g_pt_max = MAX_VTX;     /* 那块数组能装多少 */

/* 让 Push 改攒世界那块 / 改回每帧那块 */
static void PushTargetWorld(void)
{
    g_pt_dst = g_wbuf;
    g_pt_n   = &g_wn;
    g_pt_max = MAX_WORLD_VTX;
}

static void PushTargetFrame(void)
{
    g_pt_dst = g_vbuf_a;
    g_pt_n   = &g_vn;
    g_pt_max = MAX_VTX;
}

static void Push(const XMFLOAT3 &p, const XMFLOAT3 &n, const XMFLOAT3 &c)
{
    if (*g_pt_n < g_pt_max) {
        g_pt_dst[*g_pt_n].pos = p;
        g_pt_dst[*g_pt_n].nrm = n;
        g_pt_dst[*g_pt_n].col = c;
        g_pt_dst[*g_pt_n].aux = g_aux;
        (*g_pt_n)++;
    }
}

/* 一个方块：中心、三边长度、颜色 */
static void AddBox(XMFLOAT3 c, float sx, float sy, float sz, XMFLOAT3 col)
{
    XMFLOAT3 h = { sx * 0.5f, sy * 0.5f, sz * 0.5f };
    XMFLOAT3 p[8];
    int i;

    for (i = 0; i < 8; i++) {
        p[i].x = c.x + ((i & 1) ? h.x : -h.x);
        p[i].y = c.y + ((i & 2) ? h.y : -h.y);
        p[i].z = c.z + ((i & 4) ? h.z : -h.z);
    }

    /* 六个面，每面两个三角形；法线直接给轴向 */
    {
        static const int f[6][4] = {
            { 1, 3, 7, 5 },   /* +X */
            { 0, 4, 6, 2 },   /* -X */
            { 2, 6, 7, 3 },   /* +Y */
            { 0, 1, 5, 4 },   /* -Y */
            { 4, 5, 7, 6 },   /* +Z */
            { 0, 2, 3, 1 }    /* -Z */
        };
        static const XMFLOAT3 fn[6] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
        };
        int k;

        for (k = 0; k < 6; k++) {
            Push(p[f[k][0]], fn[k], col);
            Push(p[f[k][1]], fn[k], col);
            Push(p[f[k][2]], fn[k], col);
            Push(p[f[k][0]], fn[k], col);
            Push(p[f[k][2]], fn[k], col);
            Push(p[f[k][3]], fn[k], col);
        }
    }
}

/* 骨头（宽厚分开给）：掌心是扁的，手指是细的，都用它 */
static void AddBone2(XMFLOAT3 a, XMFLOAT3 b, float ws, float wu, XMFLOAT3 upHint, XMFLOAT3 col)
{
    XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
    XMVECTOR dir = XMVector3Normalize(vb - va);
    XMVECTOR up = XMLoadFloat3(&upHint);
    XMVECTOR side, up2;
    XMFLOAT3 sd, ud, md, md0;
    float len = XMVectorGetX(XMVector3Length(vb - va));
    int i, k;
    XMFLOAT3 p[8];

    if (len < 0.0005f)
        return;
    /* 骨头要是基本竖直的，(0,1,0) 和它叉乘会得到零向量 → 整块会消失，
     * 所以竖直时换个参考轴（头、脖子、胸、腰、胯都是竖直的，全靠这一句） */
    if (fabsf(XMVectorGetY(dir)) > 0.95f)
        up = XMVectorSet(0, 0, 1, 0);
    side = XMVector3Normalize(XMVector3Cross(up, dir));
    up2  = XMVector3Normalize(XMVector3Cross(dir, side));
    XMStoreFloat3(&sd, side * (ws * 0.5f));
    XMStoreFloat3(&ud, up2 * (wu * 0.5f));
    XMStoreFloat3(&md, dir * (len * 0.5f));
    XMStoreFloat3(&md0, (va + vb) * 0.5f);

    for (i = 0; i < 8; i++) {
        float sx = (i & 1) ? 1.0f : -1.0f;
        float sy = (i & 2) ? 1.0f : -1.0f;
        float sz = (i & 4) ? 1.0f : -1.0f;

        p[i].x = md0.x + sx * sd.x + sy * ud.x + sz * md.x;
        p[i].y = md0.y + sx * sd.y + sy * ud.y + sz * md.y;
        p[i].z = md0.z + sx * sd.z + sy * ud.z + sz * md.z;
    }
    for (k = 0; k < 6; k++) {
        static const int f[6][4] = {
            { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
            { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 }
        };

        Push(p[f[k][0]], upHint, col); Push(p[f[k][1]], upHint, col); Push(p[f[k][2]], upHint, col);
        Push(p[f[k][0]], upHint, col); Push(p[f[k][2]], upHint, col); Push(p[f[k][3]], upHint, col);
    }
}

/* 两点之间的一根"骨头"：盒子沿着方向摆，腿和小臂都用它 */
static void AddBone(XMFLOAT3 a, XMFLOAT3 b, float w, XMFLOAT3 col)
{
    XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
    XMVECTOR dir = XMVector3Normalize(vb - va);
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR side, up2, mid;
    XMFLOAT3 sd, ud, md, md0;
    float len = XMVectorGetX(XMVector3Length(vb - va));
    int i, k;
    XMFLOAT3 p[8];

    if (len < 0.001f)
        return;
    if (fabsf(XMVectorGetY(dir)) > 0.95f)              /* 朝上/下时换个参考轴 */
        up = XMVectorSet(0, 0, 1, 0);
    side = XMVector3Normalize(XMVector3Cross(up, dir));
    up2  = XMVector3Normalize(XMVector3Cross(dir, side));
    mid  = (va + vb) * 0.5f;
    XMStoreFloat3(&sd, side * (w * 0.5f));
    XMStoreFloat3(&ud, up2 * (w * 0.5f));
    XMStoreFloat3(&md, dir * (len * 0.5f));
    XMStoreFloat3(&md0, mid);

    for (i = 0; i < 8; i++) {
        float sx = (i & 1) ? 1.0f : -1.0f;
        float sy = (i & 2) ? 1.0f : -1.0f;
        float sz = (i & 4) ? 1.0f : -1.0f;

        p[i].x = md0.x + sx * sd.x + sy * ud.x + sz * md.x;
        p[i].y = md0.y + sx * sd.y + sy * ud.y + sz * md.y;
        p[i].z = md0.z + sx * sd.z + sy * ud.z + sz * md.z;
    }
    {
        static const int f[6][4] = {
            { 1, 3, 7, 5 }, { 0, 4, 6, 2 }, { 2, 6, 7, 3 },
            { 0, 1, 5, 4 }, { 4, 5, 7, 6 }, { 0, 2, 3, 1 }
        };
        for (k = 0; k < 6; k++) {
            Push(p[f[k][0]], { 0, 1, 0 }, col);
            Push(p[f[k][1]], { 0, 1, 0 }, col);
            Push(p[f[k][2]], { 0, 1, 0 }, col);
            Push(p[f[k][0]], { 0, 1, 0 }, col);
            Push(p[f[k][2]], { 0, 1, 0 }, col);
            Push(p[f[k][3]], { 0, 1, 0 }, col);
        }
    }
}


/* 一根圆管（可以一头粗一头细），两端带半球盖 —— 四肢、手指都用它 */
static void AddTube(XMFLOAT3 a, XMFLOAT3 b, float ra, float rb, XMFLOAT3 upHint, XMFLOAT3 col)
{
    const int SEG = 12, RINGS = 4, CAP = 3;
    XMVECTOR va = XMLoadFloat3(&a), vb = XMLoadFloat3(&b);
    XMVECTOR dir = XMVector3Normalize(vb - va);
    XMVECTOR up = XMLoadFloat3(&upHint);
    XMVECTOR side, up2;
    XMFLOAT3 sd, ud, nd;
    float len = XMVectorGetX(XMVector3Length(vb - va));
    int ring, i, c;

    if (len < 0.0005f)
        return;
    if (fabsf(XMVectorGetY(dir)) > 0.95f)
        up = XMVectorSet(0, 0, 1, 0);
    side = XMVector3Normalize(XMVector3Cross(up, dir));
    up2  = XMVector3Normalize(XMVector3Cross(dir, side));
    XMStoreFloat3(&sd, side);
    XMStoreFloat3(&ud, up2);
    XMStoreFloat3(&nd, dir);

    /* 管身：一圈一圈的圆环，法线用径向 → 看着是圆的 */
    for (ring = 0; ring < RINGS; ring++) {
        float t0 = (float)ring / RINGS, t1 = (float)(ring + 1) / RINGS;
        float r0 = ra + (rb - ra) * t0, r1 = ra + (rb - ra) * t1;
        XMFLOAT3 c0, c1, n0, n1;

        c0.x = a.x + (b.x - a.x) * t0; c0.y = a.y + (b.y - a.y) * t0; c0.z = a.z + (b.z - a.z) * t0;
        c1.x = a.x + (b.x - a.x) * t1; c1.y = a.y + (b.y - a.y) * t1; c1.z = a.z + (b.z - a.z) * t1;
        XMStoreFloat3(&n0, up2); XMStoreFloat3(&n1, up2);
        (void)n0; (void)n1;
        {
            float ang[2];
            XMFLOAT3 ring0[24], ring1[24], nn0[24], nn1[24];

            for (i = 0; i < SEG; i++) {
                float al = i * (6.2831853f / SEG);

                ang[0] = al;
                nn0[i].x = sd.x * cosf(al) + ud.x * sinf(al);
                nn0[i].y = sd.y * cosf(al) + ud.y * sinf(al);
                nn0[i].z = sd.z * cosf(al) + ud.z * sinf(al);
                ring0[i].x = c0.x + nn0[i].x * r0;
                ring0[i].y = c0.y + nn0[i].y * r0;
                ring0[i].z = c0.z + nn0[i].z * r0;
                ring1[i].x = c1.x + nn0[i].x * r1;
                ring1[i].y = c1.y + nn0[i].y * r1;
                ring1[i].z = c1.z + nn0[i].z * r1;
                nn1[i] = nn0[i];
            }
            for (i = 0; i < SEG; i++) {
                int i2 = (i + 1) % SEG;

                Push(ring0[i], nn0[i], col); Push(ring1[i], nn1[i], col); Push(ring1[i2], nn1[i2], col);
                Push(ring0[i], nn0[i], col); Push(ring1[i2], nn1[i2], col); Push(ring0[i2], nn0[i2], col);
            }
        }
    }

    /* 两端的半球盖（外面还要叠球的时候就别画，免得两层表面打架出锯齿） */
    for (c = 0; g_tube_caps && c < 2; c++) {
        XMFLOAT3 cc = c ? b : a;
        XMVECTOR cn = c ? dir : -dir;
        float rr = c ? rb : ra;
        XMVECTOR cs = c ? side : -side;

        for (ring = 0; ring < CAP; ring++) {
            float p0 = (float)ring / CAP, p1 = (float)(ring + 1) / CAP;
            float s0 = cosf(p0 * 1.5707963f), s1 = cosf(p1 * 1.5707963f);
            float h0 = sinf(p0 * 1.5707963f), h1 = sinf(p1 * 1.5707963f);
            XMVECTOR d0 = XMVector3Normalize(XMVectorMultiplyAdd(cn, XMVectorReplicate(h0), XMVectorMultiply(cs, XMVectorReplicate(s0))));
            XMVECTOR d1 = XMVector3Normalize(XMVectorMultiplyAdd(cn, XMVectorReplicate(h1), XMVectorMultiply(cs, XMVectorReplicate(s1))));
            XMFLOAT3 dir0, dir1;

            XMStoreFloat3(&dir0, d0);
            XMStoreFloat3(&dir1, d1);
            for (i = 0; i < SEG; i++) {
                float a0 = i * (6.2831853f / SEG), a1 = (i + 1) * (6.2831853f / SEG);
                XMFLOAT3 q0, q1, q2, q3, m0, m1, m2, m3;

                q0.x = cc.x + sd.x * cosf(a0) * rr * s0 + ud.x * sinf(a0) * rr * s0 + nd.x * rr * h0;
                q0.y = cc.y + sd.y * cosf(a0) * rr * s0 + ud.y * sinf(a0) * rr * s0 + nd.y * rr * h0;
                q0.z = cc.z + sd.z * cosf(a0) * rr * s0 + ud.z * sinf(a0) * rr * s0 + nd.z * rr * h0;
                q1.x = cc.x + sd.x * cosf(a1) * rr * s0 + ud.x * sinf(a1) * rr * s0 + nd.x * rr * h0;
                q1.y = cc.y + sd.y * cosf(a1) * rr * s0 + ud.y * sinf(a1) * rr * s0 + nd.y * rr * h0;
                q1.z = cc.z + sd.z * cosf(a1) * rr * s0 + ud.z * sinf(a1) * rr * s0 + nd.z * rr * h0;
                q2.x = cc.x + sd.x * cosf(a0) * rr * s1 + ud.x * sinf(a0) * rr * s1 + nd.x * rr * h1;
                q2.y = cc.y + sd.y * cosf(a0) * rr * s1 + ud.y * sinf(a0) * rr * s1 + nd.y * rr * h1;
                q2.z = cc.z + sd.z * cosf(a0) * rr * s1 + ud.z * sinf(a0) * rr * s1 + nd.z * rr * h1;
                q3.x = cc.x + sd.x * cosf(a1) * rr * s1 + ud.x * sinf(a1) * rr * s1 + nd.x * rr * h1;
                q3.y = cc.y + sd.y * cosf(a1) * rr * s1 + ud.y * sinf(a1) * rr * s1 + nd.y * rr * h1;
                q3.z = cc.z + sd.z * cosf(a1) * rr * s1 + ud.z * sinf(a1) * rr * s1 + nd.z * rr * h1;

                m0 = dir0; m1 = dir0; m2 = dir1; m3 = dir1;
                Push(q0, m0, col); Push(q1, m1, col); Push(q3, m3, col);
                Push(q0, m0, col); Push(q3, m3, col); Push(q2, m2, col);
            }
        }
    }
}

/* 一个球 / 椭球 —— 头、胸、关节、手掌都用它 */
static void AddBall(XMFLOAT3 c, float rx, float ry, float rz, XMFLOAT3 col)
{
    const int NU = 14, NV = 9;
    int v, u;

    for (v = 0; v < NV; v++) {
        float p0 = 3.14159265f * (float)v / NV, p1 = 3.14159265f * (float)(v + 1) / NV;

        for (u = 0; u < NU; u++) {
            float a0 = 6.2831853f * (float)u / NU, a1 = 6.2831853f * (float)(u + 1) / NU;
            XMFLOAT3 q[4], n[4];
            int k;

            float pp[2] = { p0, p1 };
            float aa[2] = { a0, a1 };
            int idx = 0;

            for (k = 0; k < 4; k++) {
                float pp2 = pp[(k == 1 || k == 2) ? 1 : 0];
                float aa2 = aa[(k >= 2) ? 1 : 0];

                n[idx].x = sinf(pp2) * cosf(aa2);
                n[idx].y = cosf(pp2);
                n[idx].z = sinf(pp2) * sinf(aa2);
                q[idx].x = c.x + n[idx].x * rx;
                q[idx].y = c.y + n[idx].y * ry;
                q[idx].z = c.z + n[idx].z * rz;
                idx++;
            }
            Push(q[0], n[0], col); Push(q[1], n[1], col); Push(q[2], n[2], col);
            Push(q[0], n[0], col); Push(q[2], n[2], col); Push(q[3], n[3], col);
        }
    }
}





/* 一只手：圆润的手掌 + 五根手指（管状），按给定朝向摆 */
static void AddHand(XMFLOAT3 c, XMFLOAT3 f, XMFLOAT3 r, XMFLOAT3 u, float s, int side)
{
    XMFLOAT3 skin = { 0.86f, 0.68f, 0.55f };
    XMFLOAT3 q0, q1;
    int i;

    AddBall(c, 0.058f * s, 0.024f * s, 0.068f * s, skin);          /* 手掌：椭球 */

    for (i = 0; i < 4; i++) {                                      /* 四根手指 */
        float off = (-0.033f + i * 0.022f) * s;
        float ln  = (0.078f - i * 0.007f) * s;

        q0.x = c.x + r.x * off; q0.y = c.y + r.y * off; q0.z = c.z + r.z * off;
        q1.x = q0.x + f.x * ln; q1.y = q0.y + f.y * ln; q1.z = q0.z + f.z * ln;
        AddTube(q0, q1, 0.011f * s, 0.008f * s, u, skin);
    }
    {                                                              /* 拇指 */
        float off = side * 0.048f * s;

        q0.x = c.x + r.x * off * 0.7f; q0.y = c.y + r.y * off * 0.7f; q0.z = c.z + r.z * off * 0.7f;
        q1.x = q0.x + f.x * 0.050f * s + r.x * off; q1.y = q0.y + f.y * 0.050f * s + r.y * off;
        q1.z = q0.z + f.z * 0.050f * s + r.z * off;
        AddTube(q0, q1, 0.013f * s, 0.009f * s, u, skin);
    }
}

/* ---- 地形：河是一条真的凹下去的河道 ----
 * 以前河只是地面上刷了层蓝漆，人能从水上走过去。现在地面有高低：
 * 河心比岸低 RIVER_D 米，水面比岸低 |WATER_Y| 米，岸坡 RIVER_BANK 米宽。
 * 地面网格是按"沿着河 / 离河心多远"这两个方向铺的：
 * 离河近的地方一格 1 米，远处逐渐放到 25 米，所以河道看着是圆的、不是台阶。 */
#define WATER_Y     (-0.32f)     /* 水面比岸低这么多（负数是往下） */
#define RIVER_D      1.15f       /* 河心比岸低这么多 */
#define RIVER_BANK   5.0f        /* 岸坡有多宽 */

/* 离河心线的有符号距离（正负表示在河的哪一边） */
static float RiverAcross(float x, float z)
{
    float along, across, c;

    if (!g_river_on)
        return 999.0f;
    along  = g_river_dir ? z : x;
    across = g_river_dir ? x : z;
    c = g_river_c + sinf(along * g_river_freq + g_river_ph) * g_river_amp;
    return across - c;
}

/* 这一点在河里吗（水线以内） */
static int InRiver(float x, float z)
{
    return g_river_on && fabsf(RiverAcross(x, z)) < g_river_w;
}

/* 只按"离河心多远"算地面高度 —— 地面网格本来就是这么铺的 */
static float HeightFromW(float w)
{
    float d = fabsf(w), u;

    if (!g_river_on)
        return 0.0f;
    if (d < g_river_w) {                       /* 河床：中间最深，两边收到水线 */
        u = d / g_river_w;
        return WATER_Y - (RIVER_D + WATER_Y) * (1.0f - u * u);
    }
    if (d < g_river_w + RIVER_BANK) {          /* 岸坡：从水线平顺地升回地面 */
        u = (d - g_river_w) / RIVER_BANK;
        return WATER_Y * (1.0f - u * u * (3.0f - 2.0f * u));
    }
    return 0.0f;                               /* 平地 */
}

/* 任意一点的地面高度（草、花、石头、人都要用它站上去） */
static float GroundH(float x, float z)
{
    return HeightFromW(RiverAcross(x, z));
}

/* "沿着河" 和 "离河心多远" 换算成世界坐标 */
static void AlongWToXZ(float along, float w, float *x, float *z)
{
    float c = g_river_on ? g_river_c + sinf(along * g_river_freq + g_river_ph) * g_river_amp
                         : 0.0f;

    if (g_river_dir) {                 /* 河沿 z 流：x = 河心 + 距离 */
        *x = c + w;
        *z = along;
    } else {                           /* 河沿 x 流 */
        *z = c + w;
        *x = along;
    }
}

/* 天上那个太阳：一大块圆盘，挂在固定方向的远处；不吃光照，永远亮 */
/* 太阳：中心一个小亮盘，外面一圈圈套着的环带，颜色由亮黄渐变到天空色。
 * 最外圈和天空一个颜色，所以看不见硬边——不是一块贴上去的圆饼。 */
static void AddSun(COLORREF sky_now)
{
    static const float radius[7] = { 7.0f, 11.0f, 15.0f, 20.0f, 26.0f, 33.0f, 42.0f };
    XMVECTOR dir = XMVector3Normalize(XMVectorSet(0.30f, 0.62f, 0.72f, 0));
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR side = XMVector3Normalize(XMVector3Cross(up, dir));
    XMVECTOR up2 = XMVector3Normalize(XMVector3Cross(dir, side));
    /* 太阳挂在远处：位置是"人头顶上 + 固定方向 × 320 米"，
     * 所以走到哪儿它都在天上同一个方向（地图是无限的，它不能钉在世界原点） */
    XMVECTOR c = XMVectorSet(g_eye.x, 0.0f, g_eye.z, 0.0f) + dir * 320.0f;
    XMFLOAT3 cd, sd3, ud3, nd3;
    XMFLOAT3 core = { 1.00f, 0.97f, 0.84f };              /* 中心亮黄白 */
    float sky[3];
    int ring, j;

    XMStoreFloat3(&cd, c);
    XMStoreFloat3(&sd3, side);
    XMStoreFloat3(&ud3, up2);
    XMStoreFloat3(&nd3, -dir);
    sky[0] = GetRValue(sky_now) / 255.0f;                 /* 外圈直接用当前天空色 */
    sky[1] = GetGValue(sky_now) / 255.0f;
    sky[2] = GetBValue(sky_now) / 255.0f;

    g_aux.x = -1.0f;                                      /* 不吃光照 */
    g_aux.y = 0.0f;

    for (ring = 0; ring < 6; ring++) {
        float r0 = radius[ring], r1 = radius[ring + 1];
        float t = (float)ring / 5.0f;                     /* 0 = 最里面，1 = 最外面 */
        XMFLOAT3 col;

        t = t * t * (3.0f - 2.0f * t);                    /* 平滑一点 */
        col.x = core.x + (sky[0] - core.x) * t;
        col.y = core.y + (sky[1] - core.y) * t;
        col.z = core.z + (sky[2] - core.z) * t;

        for (j = 0; j < 32; j++) {
            float a0 = 6.2831853f * j / 32.0f, a1 = 6.2831853f * (j + 1) / 32.0f;
            XMFLOAT3 p0, p1, q0, q1;

#define RINGPT(R, A, OUT)                                                        \
            OUT.x = cd.x + (sd3.x * cosf(A) + ud3.x * sinf(A)) * (R);            \
            OUT.y = cd.y + (sd3.y * cosf(A) + ud3.y * sinf(A)) * (R);            \
            OUT.z = cd.z + (sd3.z * cosf(A) + ud3.z * sinf(A)) * (R)

            RINGPT(r0, a0, p0); RINGPT(r0, a1, p1);
            RINGPT(r1, a0, q0); RINGPT(r1, a1, q1);
#undef RINGPT

            Push(p0, nd3, col); Push(q0, nd3, col); Push(q1, nd3, col);
            Push(p0, nd3, col); Push(q1, nd3, col); Push(p1, nd3, col);
        }
    }
    /* 中心的小亮盘 */
    for (j = 0; j < 32; j++) {
        float a0 = 6.2831853f * j / 32.0f, a1 = 6.2831853f * (j + 1) / 32.0f;
        XMFLOAT3 p0, p1;

        p0.x = cd.x + (sd3.x * cosf(a0) + ud3.x * sinf(a0)) * radius[0];
        p0.y = cd.y + (sd3.y * cosf(a0) + ud3.y * sinf(a0)) * radius[0];
        p0.z = cd.z + (sd3.z * cosf(a0) + ud3.z * sinf(a0)) * radius[0];
        p1.x = cd.x + (sd3.x * cosf(a1) + ud3.x * sinf(a1)) * radius[0];
        p1.y = cd.y + (sd3.y * cosf(a1) + ud3.y * sinf(a1)) * radius[0];
        p1.z = cd.z + (sd3.z * cosf(a1) + ud3.z * sinf(a1)) * radius[0];
        Push(cd, nd3, core); Push(p0, nd3, core); Push(p1, nd3, core);
    }
    g_aux.x = 0.0f;
}

static void BuildWoman(XMFLOAT3 at, float yaw);

/* ================= 无限地图 =================
 *
 * 世界不是开局建一次就不管了：它跟着人走。玩家周围半径 WORLD_R 米里的
 * 地面、草、花、树攒成一个顶点集；走到离上次的中心 WORLD_SNAP 米以外，
 * 就重新攒一遍 —— 但攒的过程分帧干（每帧最多 WORLD_BUDGET 个顶点），
 * 所以走路不会卡。攒的时候画的还是旧的那一份（双缓冲），攒完一换就完事。
 *
 * 所有东西都是"世界坐标的函数"：草、花、树的位置由所在格子的坐标哈希算出来，
 * 河由 InRiver() 算出来。所以重攒一遍，东西还长在原来的地方，一点都不会变样；
 * 树的位置也是同一套算法，碰撞箱和看见的树永远对得上。
 */

#define WORLD_R       220.0f     /* 世界以人为中心铺多大一圈 */
#define WORLD_SNAP     24.0f     /* 走出这么远就重攒一遍 */
#define TILE           16.0f     /* 草/花/树按这个尺寸的格子撒 */
#define GROUND_STEP     5.0f     /* 地面网格 */
#define WORLD_BUDGET  14000      /* 每帧最多攒多少个顶点 */
#define TREE_R         0.55f     /* 树干的碰撞半径 */
#define FOG_START      95.0f     /* 雾从多远开始 */
#define FOG_END       165.0f     /* 到多远完全化进天空色。
                                    WORLD_R - WORLD_SNAP = 196 > 165，
                                    所以地面还没铺到头就已经被雾吃掉了，看不到边 */

/* 现在这一版世界的中心（也是正在攒的那一版的）：草和花的疏密按它算 */
static float g_wcx = 0.0f, g_wcz = 0.0f;

/* 小哈希：同一个 (x,z) 永远摇出同一串随机数 */
static unsigned Hash2(int x, int z, unsigned salt)
{
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)z * 19349663u ^ salt;

    h ^= h >> 15; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h ? h : 0x9E3779B9u;
}

static float Rnd(unsigned *s)                       /* 0 ~ 1 */
{
    *s = *s * 1664525u + 1013904223u;
    return (float)((*s >> 8) & 0xFFFFFFu) / 16777215.0f;
}

static float RndR(unsigned *s, float a, float b)    /* a ~ b */
{
    return a + Rnd(s) * (b - a);
}

/* 三个浮点数顺手打包成一个向量（省得每处都写一遍成员） */
static XMFLOAT3 V3(float x, float y, float z)
{
    XMFLOAT3 v = { x, y, z };

    return v;
}

static XMFLOAT3 Norm3(XMFLOAT3 v)
{
    float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);

    if (l < 0.000001f)
        return V3(0.0f, 1.0f, 0.0f);
    return V3(v.x / l, v.y / l, v.z / l);
}

/* 一个低面数的球：树冠用（面数可以调，树冠用粗一点的省钱） */
static void AddBlobN(XMFLOAT3 c, float rx, float ry, float rz, XMFLOAT3 col, int NU, int NV)
{
    int v, u;

    for (v = 0; v < NV; v++) {
        float p0 = 3.14159265f * (float)v / NV, p1 = 3.14159265f * (float)(v + 1) / NV;

        for (u = 0; u < NU; u++) {
            float a0 = 6.2831853f * (float)u / NU, a1 = 6.2831853f * (float)(u + 1) / NU;
            XMFLOAT3 q[4], n[4];
            float pp[2] = { p0, p1 }, aa[2] = { a0, a1 };
            int k;

            for (k = 0; k < 4; k++) {
                float p2 = pp[(k == 1 || k == 2) ? 1 : 0];
                float a2 = aa[(k >= 2) ? 1 : 0];

                n[k].x = sinf(p2) * cosf(a2);
                n[k].y = cosf(p2);
                n[k].z = sinf(p2) * sinf(a2);
                q[k].x = c.x + n[k].x * rx;
                q[k].y = c.y + n[k].y * ry;
                q[k].z = c.z + n[k].z * rz;
            }
            Push(q[0], n[0], col); Push(q[1], n[1], col); Push(q[2], n[2], col);
            Push(q[0], n[0], col); Push(q[2], n[2], col); Push(q[3], n[3], col);
        }
    }
}

/* 石头用的：稍微圆一点 */
static void AddBlob(XMFLOAT3 c, float rx, float ry, float rz, XMFLOAT3 col)
{
    AddBlobN(c, rx, ry, rz, col, 7, 4);
}

/* 树冠用的：面数少，种得密也不怕 */
static void AddLeaf(XMFLOAT3 c, float rx, float ry, float rz, XMFLOAT3 col)
{
    AddBlobN(c, rx, ry, rz, col, 6, 3);
}

/* 一根没有端盖的低面数圆柱：树干、树枝用 */
static void AddStem(XMFLOAT3 a, XMFLOAT3 b, float ra, float rb, XMFLOAT3 col)
{
    const int SEG = 7;
    XMFLOAT3 sd, ud;
    float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
    float dx, dy, dz, hx, hz;
    int i;

    if (len < 0.001f)
        return;
    dx = (b.x - a.x) / len; dy = (b.y - a.y) / len; dz = (b.z - a.z) / len;
    /* 找一个和它垂直的方向 */
    if (fabsf(dy) > 0.95f) { hx = 1.0f; hz = 0.0f; } else { hx = -dz; hz = dx; }
    {
        float hl = sqrtf(hx * hx + hz * hz);

        hx /= hl; hz /= hl;
    }
    sd.x = hx; sd.y = 0.0f; sd.z = hz;
    {
        float l2 = sqrtf(sd.x * sd.x + sd.y * sd.y + sd.z * sd.z);

        sd.x /= l2; sd.y /= l2; sd.z /= l2;
    }
    ud.x = dy * sd.z - dz * sd.y;
    ud.y = dz * sd.x - dx * sd.z;
    ud.z = dx * sd.y - dy * sd.x;

    for (i = 0; i < SEG; i++) {
        float a0 = 6.2831853f * i / SEG, a1 = 6.2831853f * (i + 1) / SEG;
        XMFLOAT3 n0, n1, p0, p1, p2, p3;

        n0.x = sd.x * cosf(a0) + ud.x * sinf(a0);
        n0.y = sd.y * cosf(a0) + ud.y * sinf(a0);
        n0.z = sd.z * cosf(a0) + ud.z * sinf(a0);
        n1.x = sd.x * cosf(a1) + ud.x * sinf(a1);
        n1.y = sd.y * cosf(a1) + ud.y * sinf(a1);
        n1.z = sd.z * cosf(a1) + ud.z * sinf(a1);

        p0.x = a.x + n0.x * ra; p0.y = a.y + n0.y * ra; p0.z = a.z + n0.z * ra;
        p1.x = b.x + n0.x * rb; p1.y = b.y + n0.y * rb; p1.z = b.z + n0.z * rb;
        p2.x = a.x + n1.x * ra; p2.y = a.y + n1.y * ra; p2.z = a.z + n1.z * ra;
        p3.x = b.x + n1.x * rb; p3.y = b.y + n1.y * rb; p3.z = b.z + n1.z * rb;

        Push(p0, n0, col); Push(p1, n0, col); Push(p3, n1, col);
        Push(p0, n0, col); Push(p3, n1, col); Push(p2, n1, col);
    }
}

/* 一个低面数的锥（底面一圈 + 顶点）：松树的树冠用 */
static void AddCone(XMFLOAT3 base, XMFLOAT3 tip, float r, XMFLOAT3 col)
{
    const int SEG = 7;
    XMFLOAT3 n0, p0, p1;
    float ang;
    int i;

    for (i = 0; i < SEG; i++) {
        float a0 = 6.2831853f * i / SEG, a1 = 6.2831853f * (i + 1) / SEG;
        XMFLOAT3 q0 = { base.x + cosf(a0) * r, base.y, base.z + sinf(a0) * r };
        XMFLOAT3 q1 = { base.x + cosf(a1) * r, base.y, base.z + sinf(a1) * r };

        /* 侧面：法线朝外斜向上 */
        ang = (a0 + a1) * 0.5f;
        n0 = V3(cosf(ang), 0.45f, sinf(ang));
        {
            float l = sqrtf(n0.x * n0.x + n0.y * n0.y + n0.z * n0.z);

            n0.x /= l; n0.y /= l; n0.z /= l;
        }
        Push(q0, n0, col); Push(tip, n0, col); Push(q1, n0, col);
        /* 底面（从下往上看也不漏） */
        n0 = V3(0.0f, -1.0f, 0.0f);
        Push(base, n0, col); Push(q1, n0, col); Push(q0, n0, col);
    }
    (void)p0; (void)p1;
}

/* 一棵树。同一个 scale 下也分三种长相，加上颜色深浅，一眼看去不重样：
 *   0 = 圆冠阔叶（一团一团的树冠）
 *   1 = 尖顶松树（一层一层往上收的锥）
 *   2 = 矮胖灌木（几乎没树干，一大团）
 * 原点在树根。 */
static void AddTree(float x, float z, float scale, unsigned *sd)
{
    int kind = (int)(Rnd(sd) * 2.999f);
    float dark = RndR(sd, 0.78f, 1.22f);                 /* 每棵树颜色深浅不一样 */
    XMFLOAT3 bark  = { 0.30f * dark, 0.22f * dark, 0.14f * dark };
    XMFLOAT3 leaf1 = { 0.15f * dark, 0.40f * dark, 0.13f * dark };
    XMFLOAT3 leaf2 = { 0.21f * dark, 0.52f * dark, 0.17f * dark };
    int i;

    if (kind == 1) {
        /* 松树：直树干 + 四层锥，越往上越小（高的能有十几米） */
        float h = RndR(sd, 8.5f, 13.0f) * scale;
        float r = 0.17f * h;
        XMFLOAT3 a = { x, 0.0f, z };
        XMFLOAT3 b = { x + RndR(sd, -0.10f, 0.10f), h, z + RndR(sd, -0.10f, 0.10f) };

        AddStem(a, b, 0.030f * h, 0.013f * h, bark);
        for (i = 0; i < 4; i++) {
            float t0 = 0.20f + i * 0.18f;                /* 这四层的底在哪 */
            float y0 = h * t0, y1 = h * (t0 + 0.34f);
            float r0 = r * (1.0f - i * 0.20f);

            AddCone(V3(b.x, y0, b.z), V3(b.x, y1, b.z), r0, (i & 1) ? leaf1 : leaf2);
        }
    } else if (kind == 2) {
        /* 小树：矮一点但也有模有样，不是贴地的灌木 */
        float h = RndR(sd, 3.5f, 5.5f) * scale;
        float rr = 0.30f * h;
        XMFLOAT3 a = { x, 0.0f, z };
        XMFLOAT3 b = { x + RndR(sd, -0.10f, 0.10f), h * 0.62f, z + RndR(sd, -0.10f, 0.10f) };

        AddStem(a, b, 0.030f * h, 0.022f * h, bark);
        for (i = 0; i < 3; i++) {
            float ang = 6.2831853f * i / 3.0f + RndR(sd, -0.5f, 0.5f);
            float off = RndR(sd, 0.30f, 0.55f) * rr;

            AddLeaf(V3(x + cosf(ang) * off, h * 0.62f + RndR(sd, -0.05f, 0.25f) * h, z + sinf(ang) * off),
                    RndR(sd, 0.62f, 0.92f) * rr, RndR(sd, 0.50f, 0.75f) * rr,
                    RndR(sd, 0.62f, 0.92f) * rr, (i & 1) ? leaf1 : leaf2);
        }
    } else {
        /* 圆冠阔叶：直树干 + 几根斜枝 + 一大蓬树冠（最典型的那种大树） */
        float h  = RndR(sd, 6.5f, 10.5f) * scale;
        float r0 = 0.032f * h, r1 = 0.45f * r0;
        float top = h * 0.78f;                 /* 树干到这儿，上面全是树冠 */
        float rr = 0.34f * h;                  /* 树冠多大 */
        XMFLOAT3 trunk_a = { x, 0.0f, z };
        XMFLOAT3 trunk_b = { x + RndR(sd, -0.15f, 0.15f), top, z + RndR(sd, -0.15f, 0.15f) };

        AddStem(trunk_a, trunk_b, r0, r1, bark);

        for (i = 0; i < 3; i++) {                   /* 斜枝 */
            float ang = 6.2831853f * i / 3.0f + RndR(sd, -0.5f, 0.5f);
            float len = RndR(sd, 0.35f, 0.55f) * rr;
            XMFLOAT3 a = { x + (trunk_b.x - x) * 0.8f, top * 0.78f, z + (trunk_b.z - z) * 0.8f };
            XMFLOAT3 b = { a.x + cosf(ang) * len, a.y + RndR(sd, 0.25f, 0.5f) * rr, a.z + sinf(ang) * len };

            AddStem(a, b, 0.20f * r1, 0.12f * r1, bark);
        }

        for (i = 0; i < 5; i++) {                   /* 五团树冠，错开一点，看着蓬松 */
            float ang = 6.2831853f * i / 5.0f * 2.0f + RndR(sd, -0.4f, 0.4f);
            float off = (i == 0) ? 0.0f : RndR(sd, 0.35f, 0.70f) * rr;
            float yoff = (i == 0) ? RndR(sd, 0.15f, 0.25f) : RndR(sd, -0.25f, 0.35f);
            XMFLOAT3 c = { x + cosf(ang) * off, top + yoff * rr, z + sinf(ang) * off };

            AddLeaf(c, RndR(sd, 0.55f, 0.78f) * rr, RndR(sd, 0.44f, 0.64f) * rr,
                    RndR(sd, 0.55f, 0.78f) * rr, (i & 1) ? leaf1 : leaf2);
        }
    }
}


/* 一块石头 / 一颗石子：低面数的疙瘩，压扁一点，看着像石头不像球 */
static void AddStone(float x, float z, float s, XMFLOAT3 col, unsigned *sd)
{
    float y = GroundH(x, z);
    float rx = s * RndR(sd, 0.8f, 1.25f);
    float ry = s * RndR(sd, 0.60f, 0.95f);
    float rz = s * RndR(sd, 0.8f, 1.25f);
    XMFLOAT3 c = V3(x, y + ry * 0.35f, z);      /* 一部分埋在地里 */

    AddBlob(c, rx, ry, rz, col);
}

/* 一颗小石子：四棱小锥（18 个顶点），撒得多也不心疼 */
static void AddPebble(float x, float z, float s, XMFLOAT3 col, unsigned *sd)
{
    float y = GroundH(x, z);
    float a = RndR(sd, 0.0f, 6.2831853f);
    float r = s * RndR(sd, 0.75f, 1.25f);
    float h = s * RndR(sd, 0.7f, 1.25f);
    float ca = cosf(a), sa = sinf(a);
    XMFLOAT3 b0 = V3(x + ca * r * 1.05f, y - s * 0.15f, z + sa * r * 1.05f);
    XMFLOAT3 b1 = V3(x - sa * r * 0.85f, y - s * 0.15f, z + ca * r * 0.85f);
    XMFLOAT3 b2 = V3(x - ca * r * 1.05f, y - s * 0.15f, z - sa * r * 1.05f);
    XMFLOAT3 b3 = V3(x + sa * r * 0.85f, y - s * 0.15f, z - ca * r * 0.85f);
    XMFLOAT3 tp = V3(x + RndR(sd, -0.25f, 0.25f) * r, y + h, z + RndR(sd, -0.25f, 0.25f) * r);
    XMFLOAT3 up = V3(0.0f, 1.0f, 0.0f);

    Push(b0, up, col); Push(b1, up, col); Push(tp, up, col);
    Push(b1, up, col); Push(b2, up, col); Push(tp, up, col);
    Push(b2, up, col); Push(b3, up, col); Push(tp, up, col);
    Push(b3, up, col); Push(b0, up, col); Push(tp, up, col);
    /* 底面：埋在地里，但水边露出来的时候不会漏空 */
    Push(b0, up, col); Push(b2, up, col); Push(b1, up, col);
    Push(b0, up, col); Push(b3, up, col); Push(b2, up, col);
}

/* 这个格子里有没有树？有就返回 1，并把位置和大小写出来。
 * 树单独用一条随机串（和草、花的条数没关系），所以重攒世界的时候
 * 草可以随距离变稀，树的位置一个字节都不会动 —— 碰撞箱和看见的树永远对得上。 */
static int TileTree(int tx, int tz, XMFLOAT3 *at, float *scale)
{
    unsigned sd = Hash2(tx, tz, 0x51ED2701u);
    float x0 = tx * TILE, z0 = tz * TILE;

    if (Rnd(&sd) > 0.45f)                /* 四成半的格子里有一棵：一片一片的树林 */
        return 0;
    at->x = x0 + RndR(&sd, 2.0f, TILE - 2.0f);
    at->y = 0.0f;
    at->z = z0 + RndR(&sd, 2.0f, TILE - 2.0f);
    *scale = RndR(&sd, 0.75f, 1.35f);    /* 都是大树，只是有大有小 */
    /* 河床和岸坡上不长树（要长在平地上） */
    if (GroundH(at->x, at->z) != 0.0f || GroundH(at->x + 2.0f, at->z) != 0.0f ||
        GroundH(at->x - 2.0f, at->z) != 0.0f || GroundH(at->x, at->z + 2.0f) != 0.0f ||
        GroundH(at->x, at->z - 2.0f) != 0.0f)
        return 0;
    return 1;
}

/* 一格（TILE×TILE 米）里想要几根草/几朵花：离中心越远越稀。
 * 小数部分按概率进位，这样密度是平滑变化的，不会一圈一圈跳。 */
static int TileCount(int tx, int tz, float dist, float maxd, float per_tile, float power, unsigned salt)
{
    float fall = (dist > maxd) ? 0.0f : powf(1.0f - dist / maxd, power);
    float want = per_tile * fall;
    int   n = (int)want;
    unsigned sd = Hash2(tx, tz, salt);

    if (Rnd(&sd) < want - (float)n)
        n++;
    return n;
}

/* 攒一个格子的内容：草、花、树。
 * 每一根草、每一朵花都用"格子坐标 + 第几根"去哈希，而不是顺着一条随机串往下摇，
 * 所以远处变稀（少几根）不会让剩下的草换位置 —— 重攒世界的时候地上不长腿。 */
static void GenTile(int tx, int tz)
{
    float x0 = tx * TILE, z0 = tz * TILE;
    float cx = x0 + TILE * 0.5f, cz = z0 + TILE * 0.5f;
    float dx = cx - g_wcx, dz = cz - g_wcz;          /* 离世界的中心多远 */
    float dist = sqrtf(dx * dx + dz * dz);
    int   i, n;

    /* ---- 草：一片叶子就是一个细三角形，正反两面各一个 ---- */
    n = TileCount(tx, tz, dist, 135.0f, 800.0f, 1.8f, 0x1234ABCDu);
    for (i = 0; i < n; i++) {
        unsigned sd = Hash2(tx * 4096 + i, tz, 0x6B43A9F1u);
        float gx = x0 + Rnd(&sd) * TILE;
        float gz = z0 + Rnd(&sd) * TILE;
        float hgt = RndR(&sd, 0.26f, 0.64f);
        float wid = RndR(&sd, 0.032f, 0.072f);
        float lean = RndR(&sd, -0.15f, 0.15f);
        float dark = RndR(&sd, 0.72f, 1.27f);
        float gy;
        XMFLOAT3 gc, n2;
        float ang, dx2, dz2;

        if (InRiver(gx, gz))
            continue;                              /* 水里不长草 */
        gy = GroundH(gx, gz);                      /* 站在地面上（岸边是斜的） */
        if (gy < WATER_Y + 0.02f)
            continue;                              /* 水线以下也不长 */
        gc = V3(0.20f * dark, 0.60f * dark, 0.16f * dark);
        n2 = V3(0.0f, 1.0f, 0.0f);
        ang = 6.2831853f * ((float)(i % 16) / 16.0f + Rnd(&sd) * 0.1f);
        dx2 = cosf(ang) * wid;
        dz2 = sinf(ang) * wid;

        Push(V3(gx - dx2, gy + 0.01f, gz - dz2), n2, gc);
        Push(V3(gx + dx2, gy + 0.01f, gz + dz2), n2, gc);
        Push(V3(gx + lean * hgt, gy + hgt + 0.01f, gz + lean * hgt * 0.6f), n2, gc);
        Push(V3(gx + dx2, gy + 0.01f, gz + dz2), n2, gc);
        Push(V3(gx - dx2, gy + 0.01f, gz - dz2), n2, gc);
        Push(V3(gx + lean * hgt, gy + hgt + 0.01f, gz + lean * hgt * 0.6f), n2, gc);
    }

    /* ---- 花：一根细杆 + 顶上一朵朝上的花（低头能看见一片颜色） ---- */
    {
        static const XMFLOAT3 petal[5] = {
            { 0.97f, 0.95f, 0.62f },     /* 白黄 */
            { 0.93f, 0.30f, 0.34f },     /* 红 */
            { 0.78f, 0.42f, 0.86f },     /* 紫 */
            { 0.99f, 0.74f, 0.24f },     /* 橙黄 */
            { 0.95f, 0.91f, 0.97f }      /* 白 */
        };

        n = TileCount(tx, tz, dist, 120.0f, 55.0f, 1.3f, 0x77C0FFEEu);
        for (i = 0; i < n; i++) {
            unsigned sd = Hash2(tx * 4096 + i, tz, 0xF10C3B27u);
            float fx = x0 + Rnd(&sd) * TILE;
            float fz = z0 + Rnd(&sd) * TILE;
            float fh = RndR(&sd, 0.20f, 0.46f);
            float fw = RndR(&sd, 0.045f, 0.085f);
            float dark = RndR(&sd, 0.85f, 1.15f);
            XMFLOAT3 pc = petal[(int)(Rnd(&sd) * 4.999f)];
            XMFLOAT3 stem = V3(0.22f * dark, 0.46f * dark, 0.16f * dark);
            XMFLOAT3 nup = V3(0.0f, 1.0f, 0.0f);
            float ang, ax, az, ty, fy;

            if (InRiver(fx, fz))
                continue;
            fy = GroundH(fx, fz);                      /* 花也长在地面上 */
            if (fy < WATER_Y + 0.02f)
                continue;
            fh += fy;
            pc = V3(pc.x * dark, pc.y * dark, pc.z * dark);
            ang = 6.2831853f * ((float)(i % 16) / 16.0f);
            ax = cosf(ang) * fw;
            az = sinf(ang) * fw;
            ty = fh + fw * 1.1f;                       /* 花心比花瓣高一点 */
            /* 杆：一片竖直的细三角（两面） */
            Push(V3(fx - ax, fy + 0.01f, fz - az), nup, stem);
            Push(V3(fx + ax, fy + 0.01f, fz + az), nup, stem);
            Push(V3(fx, fh, fz), nup, stem);
            Push(V3(fx + ax, fy + 0.01f, fz + az), nup, stem);
            Push(V3(fx - ax, fy + 0.01f, fz - az), nup, stem);
            Push(V3(fx, fh, fz), nup, stem);
            /* 花：两片竖直的小十字（从旁边看是花的侧面）…… */
            Push(V3(fx - fw * 0.7f, fh, fz), nup, pc); Push(V3(fx + fw * 0.7f, fh, fz), nup, pc); Push(V3(fx, ty, fz), nup, pc);
            Push(V3(fx + fw * 0.7f, fh, fz), nup, pc); Push(V3(fx - fw * 0.7f, fh, fz), nup, pc); Push(V3(fx, ty, fz), nup, pc);
            Push(V3(fx, fh, fz - fw * 0.7f), nup, pc); Push(V3(fx, fh, fz + fw * 0.7f), nup, pc); Push(V3(fx, ty, fz), nup, pc);
            Push(V3(fx, fh, fz + fw * 0.7f), nup, pc); Push(V3(fx, fh, fz - fw * 0.7f), nup, pc); Push(V3(fx, ty, fz), nup, pc);
            /* ……再加一片朝上的，低头看下去是一片颜色 */
            Push(V3(fx - fw * 0.9f, ty, fz - fw * 0.9f), nup, pc);
            Push(V3(fx + fw * 0.9f, ty, fz - fw * 0.9f), nup, pc);
            Push(V3(fx + fw * 0.9f, ty, fz + fw * 0.9f), nup, pc);
            Push(V3(fx - fw * 0.9f, ty, fz - fw * 0.9f), nup, pc);
            Push(V3(fx + fw * 0.9f, ty, fz + fw * 0.9f), nup, pc);
            Push(V3(fx - fw * 0.9f, ty, fz + fw * 0.9f), nup, pc);
        }
    }

    /* ---- 石头和石子：离河越近越多（河边一层石子滩，远处偶尔几块） ---- */
    {
        static const XMFLOAT3 rock[4] = {
            { 0.52f, 0.50f, 0.47f },     /* 灰 */
            { 0.44f, 0.41f, 0.37f },     /* 深灰 */
            { 0.58f, 0.52f, 0.42f },     /* 土黄 */
            { 0.47f, 0.45f, 0.44f }      /* 青灰 */
        };
        float rwd = g_river_on ? fabsf(RiverAcross(cx, cz)) : 999.0f;   /* 这格子离河心多远 */
        float sandy;                  /* 1 = 河滩上，0 = 远处草地 */
        float grey;

        sandy = 1.0f - (rwd - g_river_w) / 16.0f;      /* 离水线 16 米以内算河滩 */
        if (sandy < 0.0f) sandy = 0.0f;
        if (sandy > 1.0f) sandy = 1.0f;
        sandy = sandy * sandy;
        grey  = 0.25f + 0.75f * sandy;                 /* 远处偶尔也有几块石头 */

        /* 石子：河滩上一格 130 颗，草地上 8 颗；水底下也撒，透过水看得见 */
        n = TileCount(tx, tz, dist, 130.0f, 8.0f + 122.0f * sandy, 1.5f, 0x51A1B2C3u);
        for (i = 0; i < n; i++) {
            unsigned sd = Hash2(tx * 4096 + i, tz, 0x2B7E1516u);
            float px = x0 + Rnd(&sd) * TILE;
            float pz = z0 + Rnd(&sd) * TILE;
            float s = RndR(&sd, 0.05f, 0.16f) * (1.0f + 0.6f * sandy);
            XMFLOAT3 rc = rock[(int)(Rnd(&sd) * 3.999f)];
            float dk = RndR(&sd, 0.85f, 1.15f);

            AddPebble(px, pz, s, V3(rc.x * dk, rc.y * dk, rc.z * dk), &sd);
        }

        /* 大一点的石头：河滩上一格 1.6 块，草地上少一些 */
        n = TileCount(tx, tz, dist, 150.0f, 0.35f + 1.6f * sandy, 1.2f, 0x9E3779B9u);
        for (i = 0; i < n; i++) {
            unsigned sd = Hash2(tx * 4096 + i, tz, 0x0F1E2D3Cu);
            float sx2 = x0 + Rnd(&sd) * TILE;
            float sz2 = z0 + Rnd(&sd) * TILE;
            float s = RndR(&sd, 0.35f, 1.15f) * (0.8f + 0.5f * sandy);
            XMFLOAT3 rc = rock[(int)(Rnd(&sd) * 3.999f)];
            float dk = RndR(&sd, 0.8f, 1.15f);

            AddStone(sx2, sz2, s, V3(rc.x * dk, rc.y * dk, rc.z * dk), &sd);
        }
    }

    /* ---- 树 ---- */
    {
        XMFLOAT3 at;
        float scale;

        if (TileTree(tx, tz, &at, &scale)) {
            unsigned sd = Hash2(tx, tz, 0xA5A5A5A5u);

            AddTree(at.x, at.z, scale, &sd);
        }
    }
}

/* 撞树：站在树干里就往外推（推的方向是"离树心越来越远"那个方向）。
 * 树的位置就是从 TileTree() 里算出来的，和画在地上那棵一模一样。 */
static void TreePushOut(void)
{
    int tx = (int)floorf(g_eye.x / TILE), tz = (int)floorf(g_eye.z / TILE);
    int i, j, k;

    for (k = 0; k < 2; k++) {              /* 推两遍：夹在两棵树中间也能挤出来 */
        for (i = -1; i <= 1; i++) {
            for (j = -1; j <= 1; j++) {
                XMFLOAT3 at;
                float scale, dx, dz, d, rr;

                if (!TileTree(tx + i, tz + j, &at, &scale))
                    continue;
                dx = g_eye.x - at.x;
                dz = g_eye.z - at.z;
                d = sqrtf(dx * dx + dz * dz);
                rr = TREE_R * scale + PLAYER_R;
                if (d >= rr)
                    continue;
                if (d < 0.0001f) {
                    dx = 0.0f; dz = 1.0f; d = 1.0f;
                }
                g_eye.x = at.x + dx / d * rr;
                g_eye.z = at.z + dz / d * rr;
            }
        }
        tx = (int)floorf(g_eye.x / TILE);
        tz = (int)floorf(g_eye.z / TILE);
    }
}

/* ---------------- 世界的重建：分帧干 ----------------
 * 一版世界 = 地面 + 草 + 花 + 树 + 太阳，攒成一块顶点缓冲。
 * 攒的过程分帧（每帧最多 WORLD_BUDGET 个顶点），攒完传进另一块缓冲再换过来画，
 * 所以走路的时候不会突然卡一下。 */
static int   g_wstage;                 /* 0 = 闲；1 = 攒地面；2 = 攒格子；3 = 攒水面；4 = 传完收工 */
static int   g_wgi;                    /* 地面攒到第几行 */
static int   g_wg0, g_wg1, g_wgn;      /* （旧的地面网格，留着给日志用） */
static int   g_wti, g_wt0, g_wt1, g_wtn;   /* 内容格子：游标 / x 起点 / z 起点 / 边长 */

/* 地面网格是铺在"沿着河 / 离河心多远"这套坐标里的：
 * g_gcol[] 是横断面上"离河心多远"的分档（河边密、远处疏），
 * g_gbase/g_gstep/g_grows 是沿河方向的起点/步长/行数。 */
#define MAX_GCOL 320
static float g_gcol[MAX_GCOL];
static int   g_gncol;
static float g_gbase, g_gstep;
static int   g_grows;
static int   g_water_off, g_water_n;   /* 水面顶点在缓冲里的位置（画的时候要单独一遍） */

/* 河：六成概率有，方向/宽窄/弯曲都是随机的（--seed 给了数就用它，截图前后好对比） */
static void RiverInit(void)
{
    srand((g_dbg_seed >= 0) ? (unsigned)g_dbg_seed : (unsigned)time(NULL));
    g_river_on   = ((rand() % 100) < 65) ? 1 : 0;
    g_river_dir  = rand() & 1;
    g_river_w    = 3.5f + (float)rand() / RAND_MAX * 3.5f;
    g_river_c    = ((float)rand() / RAND_MAX - 0.5f) * 60.0f;
    g_river_amp  = 5.0f + (float)rand() / RAND_MAX * 11.0f;
    g_river_freq = 0.018f + (float)rand() / RAND_MAX * 0.022f;
    g_river_ph   = (float)rand() / RAND_MAX * 6.2831853f;
}

/* 地图上随机放一个女性角色：方向和距离都是随机的，站在 5~9 米内。
 * 方向就按镜头初始朝向（+Z）算，所以她一定在开局视野正前方，
 * 不用先转一圈去找她；--dist 可以固定距离。 */
static void PlaceWoman(void)
{
    XMFLOAT3 at;
    float dist, facing;

    dist   = (g_dbg_dist > 0.1f) ? g_dbg_dist
                                 : (5.0f + (float)rand() / RAND_MAX * 2.5f);
    facing = (float)rand() / RAND_MAX * 6.2831853f;
    at.x = 0.0f;
    at.y = 0.0f;
    at.z = (g_dbg_wz < 900.0f) ? g_dbg_wz : dist;    /* --wz 可以定死位置 */
    if (g_pose == POSE_LIE)
        at.z += 1.30f;              /* 躺下的时候身体是往 +Z 铺开的，整体往前挪 */
    if (g_dbg_wz > 900.0f) {
        /* 躲开河：只在原地附近换个角度试，不许把她挪到十几米外去 */
        int tries;

        for (tries = 0; tries < 24 && InRiver(at.x, at.z); tries++) {
            float ang = (float)tries * 1.9f;

            at.x = sinf(ang) * dist * 0.16f;     /* 只左右挪一点点，一直在视野正前方 */
            at.z = cosf(ang) * dist * 0.97f;
            if (tries == 23) {                       /* 实在躲不开就退到河后面 */
                at.x = 0.0f;
                at.z = dist + 4.0f;
            }
        }
    }
    g_woman_at = at;
    g_woman_yaw = (g_dbg_wyaw < 900.0f) ? g_dbg_wyaw : facing;
    g_woman_live = 0;   /* woman removed */
}

/* 地面的颜色：按离水面的高度来 —— 河底是湿沙，水线附近是石子滩，再往上是草 */
static XMFLOAT3 GroundCol(float y)
{
    const XMFLOAT3 wet   = { 0.30f, 0.26f, 0.19f };       /* 深水的河床（暗一点） */
    const XMFLOAT3 bed   = { 0.50f, 0.45f, 0.33f };       /* 浅水的河床：看得见沙和石子 */
    const XMFLOAT3 sand  = { 0.58f, 0.53f, 0.40f };       /* 水边的沙 */
    const XMFLOAT3 grass = { 0.24f, 0.52f, 0.18f };       /* 草 */
    float t;

    if (y < WATER_Y) {                                    /* 水线以下 */
        t = (WATER_Y - y) / 0.9f;                          /* 越深越暗 */
        if (t > 1.0f) t = 1.0f;
        return V3(bed.x + (wet.x - bed.x) * t,
                  bed.y + (wet.y - bed.y) * t,
                  bed.z + (wet.z - bed.z) * t);
    }
    if (y < WATER_Y + 0.22f) {                             /* 水线往上一点点：沙 → 草 */
        t = (y - WATER_Y) / 0.22f;
        return V3(sand.x + (grass.x - sand.x) * t,
                  sand.y + (grass.y - sand.y) * t,
                  sand.z + (grass.z - sand.z) * t);
    }
    return grass;
}

/* 地面的一行：顺着河的方向切一条断面，断面上"离河心多远"是分好档的
 * （河边 1 米一格，远处慢慢放到 25 米），所以河道是圆的不是台阶。 */
static void GenGroundRow(int row)
{
    int j;

    for (j = 0; j < g_gncol - 1; j++) {
        float w0 = g_gcol[j], w1 = g_gcol[j + 1];
        float a0 = g_gbase + (float)row * g_gstep;
        float a1 = a0 + g_gstep;
        float x0, z0, x1, z1, x2, z2, x3, z3;
        float y0 = HeightFromW(w0), y1 = HeightFromW(w1);
        XMFLOAT3 p0, p1, p2, p3, col, nrm;

        AlongWToXZ(a0, w0, &x0, &z0);
        AlongWToXZ(a1, w0, &x1, &z1);
        AlongWToXZ(a1, w1, &x2, &z2);
        AlongWToXZ(a0, w1, &x3, &z3);

        p0 = V3(x0, y0, z0);
        p1 = V3(x1, y0, z1);
        p2 = V3(x2, y1, z2);
        p3 = V3(x3, y1, z3);
        col = GroundCol((y0 + y1) * 0.5f);

        /* 法线：按断面的斜率算（平地上就是朝上） */
        nrm = V3(0.0f, 1.0f, 0.0f);
        if (y0 != y1) {
            float slope = (y1 - y0) / (w1 - w0);

            if (g_river_dir)                        /* w 方向在世界里是 x */
                nrm = Norm3(V3(-slope, 1.0f, 0.0f));
            else                                    /* w 方向在世界里是 z */
                nrm = Norm3(V3(0.0f, 1.0f, -slope));
        }
        Push(p0, nrm, col); Push(p1, nrm, col); Push(p2, nrm, col);
        Push(p0, nrm, col); Push(p2, nrm, col); Push(p3, nrm, col);
    }
}

/* 水面：河心线两边各铺几格，平平的一层（比河岸低，所以看得出是"水面"）。
 * 透明度按水的深浅给：边上浅、看得见河床，河心深、颜色更实 —— 这样才像有水。 */
static void GenWaterRow(int row)
{
    const XMFLOAT3 nrm = { 0, 1, 0 };
    const XMFLOAT3 shallow = { 0.24f, 0.48f, 0.62f };     /* 浅处偏青 */
    const XMFLOAT3 deep    = { 0.09f, 0.27f, 0.52f };     /* 深处偏深蓝 */
    float a0 = g_gbase + (float)row * g_gstep;
    float a1 = a0 + g_gstep;
    float half = g_river_w * 1.02f;                /* 稍微压到岸里一点，别留缝 */
    int j;

    for (j = 0; j < 8; j++) {
        float w0 = -half + 2.0f * half * (float)j / 8.0f;
        float w1 = -half + 2.0f * half * (float)(j + 1) / 8.0f;
        float d0 = 1.0f - fabsf(w0) / half;            /* 0 = 岸边，1 = 河心 */
        float d1 = 1.0f - fabsf(w1) / half;
        float x0, z0, x1, z1, x2, z2, x3, z3;
        XMFLOAT3 c0, c1;

        AlongWToXZ(a0, w0, &x0, &z0);
        AlongWToXZ(a1, w0, &x1, &z1);
        AlongWToXZ(a1, w1, &x2, &z2);
        AlongWToXZ(a0, w1, &x3, &z3);
        c0 = V3(shallow.x + (deep.x - shallow.x) * d0,
                shallow.y + (deep.y - shallow.y) * d0,
                shallow.z + (deep.z - shallow.z) * d0);
        c1 = V3(shallow.x + (deep.x - shallow.x) * d1,
                shallow.y + (deep.y - shallow.y) * d1,
                shallow.z + (deep.z - shallow.z) * d1);

        /* 透明度给在顶点上（aux.y）：水要清，浅处几乎全透，能看见河床的沙和石头。
         * aux.x 在这里当"这是水"的记号（-3），着色器认它。 */
        g_aux.x = -3.0f;
        g_aux.y = 0.12f + 0.26f * d0;
        Push(V3(x0, WATER_Y, z0), nrm, c0);
        Push(V3(x1, WATER_Y, z1), nrm, c0);
        g_aux.y = 0.12f + 0.26f * d1;
        Push(V3(x2, WATER_Y, z2), nrm, c1);
        g_aux.y = 0.12f + 0.26f * d0;
        Push(V3(x0, WATER_Y, z0), nrm, c0);
        g_aux.y = 0.12f + 0.26f * d1;
        Push(V3(x2, WATER_Y, z2), nrm, c1);
        Push(V3(x3, WATER_Y, z3), nrm, c1);
    }
    g_aux.x = 0.0f;
    g_aux.y = 0.0f;
}

/* 开一版新的：以 (cx,cz) 为中心 */
static void WorldBegin(float cx, float cz)
{
    g_wcx = cx;
    g_wcz = cz;
    g_wn = 0;                         /* Push 的目标由 WorldStep 借着用 */

    /* 太阳和天现在都在着色器里画（见 g_sky_hlsl），不用再堆几何体了 */

    /* 地面：铺在"沿着河 / 离河心多远"这套坐标里，横断面按距离分档 */
    {
        float u = g_river_on ? (g_river_dir ? cz : cx) : 0.0f;   /* 人在河的哪个位置 */
        float w = g_river_on ? RiverAcross(cx, cz) : 0.0f;       /* 人离河心多远 */
        float lo = w - WORLD_R, hi = w + WORLD_R;
        int n = 0;

        g_gcol[n++] = lo;
        while (g_gcol[n - 1] < hi && n < MAX_GCOL - 1) {
            float ww = g_gcol[n - 1];
            float st = (fabsf(ww) > 120.0f) ? 25.0f
                     : (fabsf(ww) > 40.0f)  ? 10.0f
                     : (fabsf(ww) > 13.0f)  ? 3.0f : 1.0f;   /* 河边细、远处粗 */

            g_gcol[n] = ww + st;
            n++;
        }
        g_gcol[n - 1] = hi;
        g_gncol = n;

        g_gbase = u - WORLD_R;            /* 沿河方向：也以人为中心铺 */
        g_grows = (int)(WORLD_R * 2.0f / 8.0f) + 2;
        g_gstep = (WORLD_R * 2.0f) / (float)g_grows;
        g_wgi = 0;
    }

    g_wt0 = (int)floorf((cx - WORLD_R) / TILE);
    g_wt1 = (int)floorf((cz - WORLD_R) / TILE);
    g_wtn = (int)(WORLD_R * 2.0f / TILE) + 2;
    g_wti = 0;
    g_wstage = 1;
}

/* 把攒好的这一版传进另一块缓冲，换过来画 */
static int WorldUpload(void)
{
    int slot = 1 - g_world_cur;
    D3D11_MAPPED_SUBRESOURCE ms;

    if (g_world_cap[slot] < g_wn) {              /* 缓冲不够大就重开一块 */
        D3D11_BUFFER_DESC bd;

        if (g_world_vb[slot])
            g_world_vb[slot]->Release();
        g_world_vb[slot] = NULL;
        g_world_cap[slot] = g_wn + g_wn / 4 + 4096;
        ZeroMemory(&bd, sizeof(bd));
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vtx) * g_world_cap[slot];
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_world_vb[slot])))
            return 0;
    }
    if (FAILED(g_ctx->Map(g_world_vb[slot], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return 0;
    memcpy(ms.pData, g_wbuf, sizeof(Vtx) * g_wn);
    g_ctx->Unmap(g_world_vb[slot], 0);
    g_world_n[slot] = g_wn;
    g_world_cur = slot;
    return 1;
}

/* 每帧叫一次：要么在攒，要么看看该不该开新的一版。
 * 注意 Push 的目标只在"这一函数里面"借给世界 —— 攒世界是跨帧的，
 * 但每帧的"自己/她/UI"也要用 Push，所以借完必须马上还，
 * 不然它们的顶点会跟着攒进世界那块里（地面会花掉、顶点数翻倍）。 */
static void WorldStep(void)
{
    UINT start;

    if (g_wstage == 0) {
        float dx = g_eye.x - g_wcx, dz = g_eye.z - g_wcz;

        if (g_world_n[g_world_cur] > 0 && dx * dx + dz * dz < WORLD_SNAP * WORLD_SNAP)
            return;                          /* 还在中心附近，不用重攒 */
        WorldBegin(g_eye.x, g_eye.z);
    }

    PushTargetWorld();
    start = g_wn;
    while (g_wn - start < WORLD_BUDGET) {
        if (g_wstage == 1) {                             /* 地面：一行一行来 */
            if (g_wgi >= g_grows) {
                g_wstage = 2;
                continue;
            }
            GenGroundRow(g_wgi);
            g_wgi++;
        } else if (g_wstage == 2) {                      /* 草/花/树/石头：一格一格来 */
            if (g_wti >= g_wtn * g_wtn) {
                g_wstage = 3;
                continue;
            }
            GenTile(g_wt0 + g_wti % g_wtn, g_wt1 + g_wti / g_wtn);
            g_wti++;
        } else if (g_wstage == 3) {                      /* 水面：一整片攒完再传 */
            int r;

            g_water_off = (int)g_wn;
            if (g_river_on) {
                for (r = 0; r < g_grows; r++)
                    GenWaterRow(r);
            }
            g_water_n = (int)g_wn - g_water_off;
            g_wstage = 4;
            continue;
        } else {
            break;
        }
    }

    if (g_wstage == 4) {
        WorldUpload();
        g_wstage = 0;
    }
    PushTargetFrame();                   /* 借完就还 */
}

/* 开局：把第一版世界一次攒完（同步，反正还没开始画） */
static void WorldBuildNow(float cx, float cz)
{
    WorldBegin(cx, cz);
    while (g_wstage)
        WorldStep();
}
/* 屏幕 UI 用的小方块：直接给镜头右/上方向上的两个角，出两个三角形，
 * 不依赖叉乘，所以竖直的笔画也不会像以前那样塌掉。 */
static void AddHudQuad(XMFLOAT3 org, XMFLOAT3 rgt, XMFLOAT3 up, XMFLOAT3 fwd,
                       float x0, float y0, float x1, float y1, XMFLOAT3 col)
{
    XMFLOAT3 n = { -fwd.x, -fwd.y, -fwd.z };
    XMFLOAT3 p[4];
    int k;

    p[0].x = org.x + rgt.x * x0 + up.x * y0; p[0].y = org.y + rgt.y * x0 + up.y * y0; p[0].z = org.z + rgt.z * x0 + up.z * y0;
    p[1].x = org.x + rgt.x * x1 + up.x * y0; p[1].y = org.y + rgt.y * x1 + up.y * y0; p[1].z = org.z + rgt.z * x1 + up.z * y0;
    p[2].x = org.x + rgt.x * x1 + up.x * y1; p[2].y = org.y + rgt.y * x1 + up.y * y1; p[2].z = org.z + rgt.z * x1 + up.z * y1;
    p[3].x = org.x + rgt.x * x0 + up.x * y1; p[3].y = org.y + rgt.y * x0 + up.y * y1; p[3].z = org.z + rgt.z * x0 + up.z * y1;
    for (k = 0; k < 6; k++)
        Push(p[k < 3 ? k : (k == 3 ? 0 : (k == 4 ? 2 : 3))], n, col);
}

/* 右上角的帧率：七段数字。每一段就是一个正矩形（横段给宽、竖段给高），
 * 不做任何朝向运算，所以一定画得出来、也不会歪。 */
static void AddDigits(const wchar_t *text, XMFLOAT3 org, XMFLOAT3 rgt, XMFLOAT3 up,
                      XMFLOAT3 fwd, float sc)
{
    static const struct { wchar_t ch; int seg; } tab[] = {
        { L'0', 0x3F }, { L'1', 0x06 }, { L'2', 0x5B }, { L'3', 0x4F }, { L'4', 0x66 },
        { L'5', 0x6D }, { L'6', 0x7D }, { L'7', 0x07 }, { L'8', 0x7F }, { L'9', 0x6F },
        { L'F', 0x71 }, { L'P', 0x73 }, { L'S', 0x6D }, { L' ', 0x00 }, { 0, 0 }
    };
    XMFLOAT3 ink = { 1.00f, 0.98f, 0.92f };
    XMFLOAT3 pan = { 0.05f, 0.06f, 0.07f };
    float w = 0.036f * sc, h = 0.072f * sc, t = 0.013f * sc, gap = 0.014f * sc;
    int len = lstrlenW(text), i2, k2;

    AddHudQuad(org, rgt, up, fwd, -gap, -gap, len * (w + gap) + gap, h + gap * 2.0f, pan);

    for (i2 = 0; i2 < len; i2++) {
        int code = 0;
        float ox = i2 * (w + gap);
        float seg[7][4] = {
            { 0.00f, h - t, w, h },
            { w - t, h * 0.5f, w, h },
            { w - t, 0.0f, w, h * 0.5f + t },
            { 0.00f, 0.0f, w, t },
            { 0.00f, 0.0f, t, h * 0.5f + t },
            { 0.00f, h * 0.5f - t, t, h },
            { 0.00f, h * 0.5f - t * 0.5f, w, h * 0.5f + t * 0.5f }
        };

        for (k2 = 0; tab[k2].ch; k2++)
            if (tab[k2].ch == text[i2])
                code = tab[k2].seg;
        for (k2 = 0; k2 < 7; k2++) {
            if (!(code & (1 << k2)))
                continue;
            AddHudQuad(org, rgt, up, fwd, ox + seg[k2][0], seg[k2][1],
                       ox + seg[k2][2], seg[k2][3], ink);
        }
    }
}

/* 两条胳膊：**长在身上**的，垂在身体两侧（肩 → 上臂 → 肘 → 小臂 → 手）。
 * 和躯干、腿一样是世界空间的，跳起来一起跳，低头就能看见手在胯两边；
 * 走路时以肩为轴前后摆（两臂反相）—— 所以它归"自己这副身子"那一遍画。 */
static void AddPlayerArms(void)
{
    float lift   = SelfLift();                      /* 脚下地面 + 跳起来的高度 */
    float sw     = sinf(g_walk) * g_walk_amp;       /* -1 ~ 1：摆动的相位 */
    float breath = sinf((float)g_clock * 1.05f) * 0.011f;
    float fx = sinf(g_yaw), fz = cosf(g_yaw);       /* 前 */
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);      /* 右 */
    XMFLOAT3 f3 = { fx, 0.0f, fz };
    XMFLOAT3 r3 = { rx, 0.0f, rz };
    XMFLOAT3 down = { 0.0f, -1.0f, 0.0f };
    XMFLOAT3 upf  = { 0.0f, 1.0f, 0.0f };
    XMFLOAT3 skin = { 0.86f, 0.68f, 0.55f };
    int side;

    for (side = -1; side <= 1; side += 2) {
        float ph = (side < 0) ? sw : -sw;           /* 左右反相 */
        XMFLOAT3 sh, el, wr, hd;

        /* 肩：肩线下面一点、左右各 20 厘米（和躯干上的肩膀对齐） */
        sh.x = g_eye.x + r3.x * (side * 0.198f);
        sh.y = SELF_EYE - 0.345f + lift + breath;
        sh.z = g_eye.z + r3.z * (side * 0.198f);

        /* 肘：自然下垂，往外挪一点点（不然手会插进胯里），前后摆一点 */
        el.x = sh.x + f3.x * (ph * 0.16f) + r3.x * (side * 0.038f);
        el.y = sh.y - 0.30f;
        el.z = sh.z + f3.z * (ph * 0.16f) + r3.z * (side * 0.038f);

        /* 手腕：小臂再往下，摆动幅度比肘大一点（像钟摆） */
        wr.x = el.x + f3.x * (ph * 0.14f) + r3.x * (side * 0.030f);
        wr.y = el.y - 0.28f;
        wr.z = el.z + f3.z * (ph * 0.14f) + r3.z * (side * 0.030f);

        /* 手：掌心朝里、手指朝下，正好垂在胯两边 */
        hd.x = wr.x + f3.x * (ph * 0.03f);
        hd.y = wr.y - 0.05f;
        hd.z = wr.z + f3.z * (ph * 0.03f);

        AddBall(sh, 0.054f, 0.054f, 0.054f, skin);              /* 肩关节 */
        AddTube(sh, el, 0.052f, 0.043f, upf, skin);             /* 上臂 */
        AddBall(el, 0.043f, 0.043f, 0.043f, skin);              /* 肘关节 */
        AddTube(el, wr, 0.043f, 0.036f, upf, skin);             /* 小臂 */
        AddHand(hd, down, r3, f3, 0.80f, side);                 /* 手 */
    }
}

/* 右上角的帧率：跟着镜头走的"视图空间"小方块，最后一个画、不吃深度 */
static void BuildHud(void)
{
    XMVECTOR up  = XMVectorSet(0, 1, 0, 0);
    XMVECTOR look = XMVectorSet(sinf(g_yaw) * cosf(g_pitch), sinf(g_pitch),
                                cosf(g_yaw) * cosf(g_pitch), 0);   /* 真正的视线方向 */
    XMVECTOR lookR = XMVector3Normalize(XMVector3Cross(up, look));
    XMVECTOR lookU = XMVector3Normalize(XMVector3Cross(look, lookR));
    XMFLOAT3 f3, r3, u3;

    XMStoreFloat3(&f3, look);
    XMStoreFloat3(&r3, lookR);
    XMStoreFloat3(&u3, lookU);

    g_vn = 0;

    {   /* 右上角的 FPS */
        static double last = -1.0;
        static int    shown = 0;
        wchar_t buf[24];

        if (g_clock - last > 0.25 || last < 0.0) {
            shown = (int)(g_fps_now + 0.5);
            last = g_clock;
        }
        wsprintfW(buf, L"%d FPS", shown);
        {
            XMFLOAT3 o, rr, uu;

            rr = r3; uu = u3;
            /* 镜头前 0.45 米，右上角 */
            o.x = g_eye.x + f3.x * 0.45f + r3.x * 0.20f + u3.x * 0.185f;
            o.y = g_eye.y + f3.y * 0.45f + r3.y * 0.20f + u3.y * 0.185f;
            o.z = g_eye.z + f3.z * 0.45f + r3.z * 0.20f + u3.z * 0.185f;
            AddDigits(buf, o, rr, uu, f3, 0.85f);
        }
    }
}

/* ---- 玩家自己的身体：低头能看见的胯、腰、胸、肩 ----
 *
 * 以前这是一根上下差不多粗的圆筒（半径 0.14~0.18，顶上还是一个平盖），
 * 从上往下看就是个桶口，所以怎么都不像人。现在按人的比例一段段收放：
 * 肩最宽、胸有胸肌、腰收进去、胯再放出来；
 * 每一圈也不是正圆——肚子往前鼓、屁股往后鼓。
 *
 * 表里的高度都是"站着时的离地高度"；跳起来的时候整张表一起抬 lift，
 * 所以是整个人（连胯带腿）跟着镜头跳，而不是只有手在动。
 *
 * 局部坐标：x = 右手边，y = 上，z = 面朝的方向（跟着镜头朝哪边转而转）。 */
static const float g_torso[][4] = {
    /*  离地高度   左右半宽   前深     后深  */
    {  0.735f,   0.148f,   0.100f,  0.096f },   /* 胯底：接大腿根 */
    {  0.800f,   0.162f,   0.108f,  0.118f },
    {  0.870f,   0.170f,   0.112f,  0.128f },   /* 胯 / 屁股：下半身最宽 */
    {  0.940f,   0.163f,   0.112f,  0.118f },
    {  1.010f,   0.150f,   0.114f,  0.104f },   /* 小肚子 */
    {  1.080f,   0.140f,   0.116f,  0.100f },   /* 腰：最细的地方 */
    {  1.150f,   0.143f,   0.119f,  0.100f },
    {  1.220f,   0.152f,   0.126f,  0.104f },   /* 胸的下沿 */
    {  1.290f,   0.170f,   0.130f,  0.108f },   /* 胸：两块胸肌长在这两圈上 */
    {  1.350f,   0.190f,   0.124f,  0.109f },
    {  1.400f,   0.204f,   0.114f,  0.106f },   /* 肩：全身最宽 */
    {  1.436f,   0.196f,   0.104f,  0.099f },   /* 肩头（三角肌那一带） */
    {  1.462f,   0.140f,   0.088f,  0.090f },   /* 斜方肌：往脖子收进去 */
    {  1.478f,   0.088f,   0.068f,  0.072f },   /* 领口：这里往下是肩膀的坡 */
    {  1.492f,   0.082f,   0.064f,  0.068f },   /* 短脖子：就 2 厘米高，
                                                   一低头能看出中间是"领口"，
                                                   再高就变成一个秃头了 */
    {  1.502f,   0.074f,   0.058f,  0.062f }
};
#define TORSO_ROWS ((int)(sizeof(g_torso) / sizeof(g_torso[0])))

/* 第 i 圈的那四个数（行号越界就夹住，差分算斜率时要用到两头） */
static void TorsoRing(int i, float *y, float *hw, float *df, float *db)
{
    if (i < 0)
        i = 0;
    if (i > TORSO_ROWS - 1)
        i = TORSO_ROWS - 1;
    *y  = g_torso[i][0];
    *hw = g_torso[i][1];
    *df = g_torso[i][2];
    *db = g_torso[i][3];
}

/* 躯干表面上的一点（局部坐标）：a = 0 在右手边，a = π/2 是正前方 */
static XMFLOAT3 TorsoLocal(int i, float a)
{
    float y, hw, df, db, dm, dd;
    float s = sinf(a), c = cosf(a);
    float bump = 0.0f;

    TorsoRing(i, &y, &hw, &df, &db);
    dm = (df + db) * 0.5f;                 /* 前后的平均深度 */
    dd = (df - db) * 0.5f;                 /* 前比后多出来多少 */

    /* 胸肌：胸口左右各鼓一块，正中间（胸骨）留一道沟，
     * 所以形状是 sin(2a)：正前方是 0，左右两侧也是 0，中间偏一点最鼓。 */
    if (s > 0.0f) {
        float u = (y - 1.19f) / 0.20f;     /* 只有胸这一段有 */

        if (u > 0.0f && u < 1.0f)
            bump = 0.026f * sinf(u * 3.14159265f) * sqrtf(s) * fabsf(sinf(a * 2.0f));
    }
    return V3(hw * c, y, (dm + dd * s) * s + bump);
}

/* 法线用差分求（沿角度和沿高度各差一下再叉乘），这样胸肌鼓包也算得准 */
static XMFLOAT3 TorsoNormal(int i, float a)
{
    const float da = 0.04f;
    XMFLOAT3 pa = TorsoLocal(i, a + da), pb = TorsoLocal(i, a - da);
    XMFLOAT3 pu = TorsoLocal(i + 1, a),  pd = TorsoLocal(i - 1, a);
    XMFLOAT3 ta = V3(pa.x - pb.x, pa.y - pb.y, pa.z - pb.z);      /* 沿角度 */
    XMFLOAT3 ty = V3(pu.x - pd.x, pu.y - pd.y, pu.z - pd.z);      /* 沿高度 */

    /* ty × ta 指向外侧 */
    return Norm3(V3(ty.y * ta.z - ty.z * ta.y,
                    ty.z * ta.x - ty.x * ta.z,
                    ty.x * ta.y - ty.y * ta.x));
}

/* 躯干局部坐标/法线 -> 世界：绕 Y 轴转到镜头朝向，再整体抬高（跳、呼吸） */
static void TorsoToWorld(XMFLOAT3 lp, XMFLOAT3 ln, float lift,
                         XMFLOAT3 *wp, XMFLOAT3 *wn)
{
    float fx = sinf(g_yaw), fz = cosf(g_yaw);        /* 前 */
    float rx = cosf(g_yaw), rz = -sinf(g_yaw);       /* 右 */

    wp->x = g_eye.x + rx * lp.x + fx * lp.z;
    wp->y = lp.y + lift;
    wp->z = g_eye.z + rz * lp.x + fz * lp.z;
    *wn = Norm3(V3(rx * ln.x + fx * ln.z, ln.y, rz * ln.x + fz * ln.z));
}

/* 呼吸只让胸口那一带动，胯不动：越靠上权重越大。
 * 封口的地方也要用同一个权重，不然接缝会差出一点点，露出一圈缝。 */
static float TorsoBreathW(float y)
{
    float w = (y - 0.80f) / 0.62f;

    if (w < 0.0f)
        w = 0.0f;
    if (w > 1.0f)
        w = 1.0f;
    return w;
}

static void AddPlayerTorso(void)
{
    XMFLOAT3 pants = { 0.30f, 0.33f, 0.28f };        /* 裤子（和腿一个色） */
    XMFLOAT3 shirt = { 0.38f, 0.46f, 0.58f };        /* 上衣 */
    const int seg = 26;
    float lift   = SelfLift();                       /* 脚下地面 + 跳起来的高度 */
    float breath = sinf((float)g_clock * 1.05f) * 0.010f;
    int i, j;

    g_aux.x = 0.0f;                                  /* 自己的身子不吃着色器里的呼吸 */
    g_aux.y = 0.0f;

    for (i = 0; i < TORSO_ROWS - 1; i++) {
        float y0 = g_torso[i][0];
        XMFLOAT3 col = (y0 < 0.985f) ? pants : shirt;

        for (j = 0; j < seg; j++) {
            float a0 = 6.2831853f * j / seg, a1 = 6.2831853f * (j + 1) / seg;
            XMFLOAT3 p[4], n[4];
            int k;

            for (k = 0; k < 4; k++) {                /* 0=(i,a0) 1=(i,a1) 2=(i+1,a1) 3=(i+1,a0) */
                int   r = (k >= 2) ? (i + 1) : i;
                float a = (k == 1 || k == 2) ? a1 : a0;

                TorsoToWorld(TorsoLocal(r, a), TorsoNormal(r, a),
                             lift + breath * TorsoBreathW(g_torso[r][0]), &p[k], &n[k]);
            }
            Push(p[0], n[0], col); Push(p[1], n[1], col); Push(p[2], n[2], col);
            Push(p[0], n[0], col); Push(p[2], n[2], col); Push(p[3], n[3], col);
        }
    }

    /* 两头封口：上面是肩膀的圆顶，下面把胯底也堵上
     * （不封的话，跳起来从下面能看进空腔里） */
    for (j = 0; j < 2; j++) {
        float a0 = 6.2831853f * 0.0f, a1 = 6.2831853f * 1.0f;
        float y = (j == 0) ? (g_torso[TORSO_ROWS - 1][0] + 0.008f) : 0.712f;
        int   ring = (j == 0) ? (TORSO_ROWS - 1) : 0;
        XMFLOAT3 c0, cn;
        XMFLOAT3 cc = (j == 0) ? shirt : pants;
        int k;

        TorsoToWorld(V3(0.0f, y, 0.0f), V3(0.0f, (j == 0) ? 1.0f : -1.0f, 0.0f),
                     lift + breath * TorsoBreathW(g_torso[ring][0]), &c0, &cn);
        for (k = 0; k < seg; k++) {
            XMFLOAT3 p0, p1, n0, n1;
            float w = lift + breath * TorsoBreathW(g_torso[ring][0]);

            a0 = 6.2831853f * k / seg;
            a1 = 6.2831853f * (k + 1) / seg;
            TorsoToWorld(TorsoLocal(ring, a0), TorsoNormal(ring, a0), w, &p0, &n0);
            TorsoToWorld(TorsoLocal(ring, a1), TorsoNormal(ring, a1), w, &p1, &n1);
            Push(c0, cn, cc);
            Push(p0, n0, cc);
            Push(p1, n1, cc);
        }
    }
}

/* 自己这副身子：躯干 + 两条胳膊 + 两条腿，全在世界空间里、跟着人走。
 * 走路时腿前后摆、胳膊前后摆；跳起来整副身子一起离地，膝盖收起来。 */
static void BuildSelf(void)
{
    XMFLOAT3 cloth = { 0.32f, 0.36f, 0.30f };     /* 裤子的颜色 */
    XMFLOAT3 shoe  = { 0.22f, 0.20f, 0.18f };
    int leg;
    float thigh = 0.46f, shin = 0.44f;
    float lift = SelfLift();                      /* 脚下地面 + 跳起来的高度 */
    float tuck;                                   /* 收腿：0 = 伸直站着，1 = 抱起来 */

    /* 身子跟着镜头转：左右方向用"右"，前后摆动用"前" */
    XMVECTOR fwd = XMVectorSet(sinf(g_yaw), 0, cosf(g_yaw), 0);
    XMVECTOR rgt = XMVectorSet(cosf(g_yaw), 0, -sinf(g_yaw), 0);
    XMFLOAT3 f3, r3, up2f = { 0, 1, 0 };

    XMStoreFloat3(&f3, fwd);
    XMStoreFloat3(&r3, rgt);

    tuck = (g_jy > 0.0f) ? (g_jy / 0.45f) : 0.0f; /* 一离地就收腿，收得越来越紧 */
    if (tuck > 1.0f)
        tuck = 1.0f;
    if (g_jvy < 0.0f)
        tuck *= 0.55f;                            /* 开始往下落了就把腿伸开一点，准备落地 */

    g_vn = 0;
    AddPlayerTorso();                 /* 躯干：胯到肩 */
    AddPlayerArms();                  /* 两条胳膊，垂在身体两侧 */
    for (leg = 0; leg < 2; leg++) {
        float side = (leg == 0) ? -0.105f : 0.105f;
        float ph = g_walk + (leg ? 3.14159265f : 0.0f);       /* 两条腿反相 */
        float sw = sinf(ph) * 0.42f * g_walk_amp;             /* 抬腿的幅度 */
        float sp = (leg == 0) ? -1.0f : 1.0f;                 /* 左腿 -1，右腿 +1 */
        float spread = g_bend_amt * 0.155f;                   /* 弯腰时两条腿岔开 */
        float t1 = sw + tuck * 0.60f;             /* 大腿和竖直方向的夹角（收腿时往前抬） */
        float t2 = sw * 0.5f + tuck * 1.05f;      /* 小腿相对大腿往后折多少 */
        XMFLOAT3 hip, knee, ank, toe;

        /* 胯跟着人走：跳起来抬 lift，弯腰的时候连着上身一起降一点 */
        {
            float by = SELF_HIP + lift, bz = 0.0f;

            if (g_bend_amt > 0.0005f) {          /* 和 W() 里一样的绕胯旋转 */
                float ang = g_bend_amt * 1.15f;
                float c = cosf(ang), s = sinf(ang);
                float dy = SELF_HIP - 0.95f, dz = 0.0f;

                by = 0.95f + dy * c - dz * s + lift;
                bz = dy * s + dz * c;
            }
            hip.x = g_eye.x + r3.x * (side + sp * spread);
            hip.y = by;
            hip.z = g_eye.z + r3.z * (side + sp * spread) + bz;
        }

        knee.x = hip.x + f3.x * (thigh * sinf(t1)) + r3.x * sp * spread * 0.30f;
        knee.y = hip.y - thigh * cosf(t1);                    /* 膝盖往前甩 */
        knee.z = hip.z + f3.z * (thigh * sinf(t1)) + r3.z * sp * spread * 0.30f;

        ank.x = hip.x + f3.x * (thigh * sinf(t1) + shin * sinf(t1 - t2))
                     + r3.x * sp * spread * 0.52f;
        ank.y = knee.y - shin * cosf(t1 - t2) + 0.045f;
        ank.z = hip.z + f3.z * (thigh * sinf(t1) + shin * sinf(t1 - t2))
                     + r3.z * sp * spread * 0.52f;

        toe.x = ank.x + f3.x * (0.13f * cosf(t1 - t2));       /* 脚尖朝前，脚背跟着小腿转 */
        toe.y = ank.y - 0.022f;
        toe.z = ank.z + f3.z * (0.13f * cosf(t1 - t2));

        AddTube(hip, knee, 0.082f, 0.060f, up2f, cloth);        /* 大腿 */
        AddBall(knee, 0.062f, 0.062f, 0.062f, cloth);
        AddTube(knee, ank, 0.060f, 0.045f, up2f, cloth);       /* 小腿 */
        AddTube(ank, toe, 0.048f, 0.038f, up2f, shoe);         /* 脚 */
    }
}


/* 绕 Y 轴转一下（给角色摆朝向用） */
static XMFLOAT3 RotY(float x, float y, float z, float yaw)
{
    float c = cosf(yaw), sn = sinf(yaw);
    XMFLOAT3 o;

    o.x = x * c + z * sn;
    o.y = y;
    o.z = -x * sn + z * c;
    return o;
}

/* BuildWoman 用：局部坐标 -> 世界坐标 */
static XMFLOAT3 g_wm_at;
static float    g_wm_yaw;

/* 大腿根：这个点长在她"自己"身上（岔开、下沉、躺下都按她的朝向来），
 * 所以只走姿势变换和转身，不跟着上身一起弯腰。 */
static XMFLOAT3 hip3(float lx, float ly, float lz)
{
    XMFLOAT3 t = RotY(lx, ly, lz, g_wm_yaw);
    XMFLOAT3 o;

    o.x = g_wm_at.x + t.x;
    o.y = t.y;
    o.z = g_wm_at.z + t.z;
    return o;
}

/* 坐下来的时候，站姿高度 y 应该落到多高。
 * 只压"腰腹"这一段：肩膀往上（脖子、头、脸）整体刚性平移下去，
 * 所以脖子该多长还是多长，头也不会缩进肩膀里 —— 之前一压到底才没了脖子。 */
static float SitDrop(float y)
{
    float k;

    if (y >= SIT_FOLD)
        return (SIT_HIP + 1.0f * (SIT_FOLD - 0.925f)) - SIT_FOLD;   /* 肩上：刚性平移 */

    k = SIT_KMIN + (1.0f - SIT_KMIN) * (y - 0.855f) / (SIT_FOLD - 0.855f);
    if (k > 1.0f) k = 1.0f;
    if (k < SIT_KMIN) k = SIT_KMIN;
    return SIT_HIP + k * (y - 0.925f) - y;
}

/* 把她"自己身上"的坐标（站姿局部坐标）摆到世界里：
 *   躺下 = 绕 X 轴转 -90°（头顶转向前方、脸朝上），再抬起来让后脑贴地；
 *   坐着 = 腰腹压一压、下沉到胯离地；站着 = 原样。 */
static XMFLOAT3 Xform(float x, float y, float z)
{
    XMFLOAT3 o;

    if (g_pose == POSE_LIE) {
        float c = cosf(LIE_ROT), s = sinf(LIE_ROT);

        o.x = x;
        o.y = LIE_LIFT + z * c - y * s;      /* 局部 z（身前）→ 世界上方 */
        o.z = z * s + y * c;                 /* 局部 y（头顶方向）→ 世界前方 */
    } else {
        o = V3(x, y, z);
    }
    return o;
}

static XMFLOAT3 W(float x, float y, float z)
{
    /* lie down: rotate the whole body backwards about the hip, then drop it */
    if (g_lie_amt > 0.0005f) {
        float la = -1.5707963f * g_lie_amt;
        float lc = cosf(la), ls = sinf(la);
        float ldy = y - 0.95f, ldz = z;

        y = 0.95f + ldy * lc - ldz * ls - 0.72f * g_lie_amt;
        z = ldy * ls + ldz * lc;
    }


    float bx = x, by = y, bz = z;
    XMFLOAT3 t, o;

    if (g_pose == POSE_SIT) {
        y += SitDrop(y);
    }
    by = y;

    if (g_pose == POSE_SIT && y > 1.30f) {   /* 坐着的时候头低一点，像在往下看 */
        float wgt = (y - 1.30f) / 0.28f;

        if (wgt > 1.0f) wgt = 1.0f;
        {
            float ang = g_pose_down * wgt;
            float c = cosf(ang), s = sinf(ang);
            float dy = y - 1.28f, dz = z;

            by = 1.28f + dy * c - dz * s;
            bz = dy * s + dz * c;
        }
    }

    if (g_bend_amt > 0.0005f) {              /* 弯腰：以胯为轴，越往上转得越多 */
        float wgt = (y - 0.95f) / 0.72f;

        if (wgt < 0.0f) wgt = 0.0f;
        if (wgt > 1.0f) wgt = 1.0f;
        wgt = wgt * wgt * (3.0f - 2.0f * wgt);
        {
            float ang = g_bend_amt * wgt * 1.15f;
            float c = cosf(ang), s = sinf(ang);
            float dy = y - 0.95f, dz = z;

            by = 0.95f + dy * c - dz * s;
            bz = dy * s + dz * c;
        }
        bx = x;
    }
    {   /* 躺下：整块转过去（头顶朝前、脸朝上） */
        XMFLOAT3 r = Xform(bx, by, bz);

        bx = r.x; by = r.y; bz = r.z;
    }
    t = RotY(bx, by, bz, g_wm_yaw);

    /* 高度一律按地面算（她就站在/坐在/躺在地上，不另外给 y 偏移） */
    o.x = g_wm_at.x + t.x;
    o.y = t.y;
    o.z = g_wm_at.z + t.z;
    return o;
}

/* 旋转体：给一条"高度 → 半径"的轮廓，绕 Y 轴转一圈。
 * 身体（胯 → 腰 → 胸 → 肩）就是这么一条连续曲线，
 * 所以上半身、肚子、下半身是同一个曲面，接得平滑、没有台阶。 */
static void AddLathe(const float prof[][2], int rows, float zsquash, int seg,
                     float aux_lo, float aux_hi, XMFLOAT3 col)
{
    int i, j;

    if (rows < 2 || seg < 3)
        return;

    for (i = 0; i < rows - 1; i++) {
        float y0 = prof[i][0], r0 = prof[i][1];
        float y1 = prof[i + 1][0], r1 = prof[i + 1][1];
        float d0 = (i > 0) ? (prof[i + 1][1] - prof[i - 1][1]) / (prof[i + 1][0] - prof[i - 1][0])
                           : (r1 - r0) / (y1 - y0);
        float d1 = (i + 1 < rows - 1)
                     ? (prof[i + 2][1] - prof[i][1]) / (prof[i + 2][0] - prof[i][0])
                     : (r1 - r0) / (y1 - y0);
        float t0 = (float)i / (rows - 1), t1 = (float)(i + 1) / (rows - 1);

        for (j = 0; j < seg; j++) {
            float a0 = 6.2831853f * j / seg, a1 = 6.2831853f * (j + 1) / seg;
            XMFLOAT3 q[4], nn[4];
            float aa[2] = { a0, a1 };
            float yy[2] = { y0, y1 };
            float rr[2] = { r0, r1 };
            float dd[2] = { d0, d1 };
            float tt[2] = { t0, t1 };
            int k;

            for (k = 0; k < 4; k++) {
                int ring = (k >= 2) ? 1 : 0;      /* 0,1 在下环；2,3 在上环 */
                int ang  = (k == 1 || k == 2) ? 1 : 0;
                XMVECTOR nv;

                {
                    float lx = cosf(aa[ang]) * rr[ring];
                    float ly = yy[ring];
                    float lz = sinf(aa[ang]) * rr[ring] * zsquash;
                    float nx = cosf(aa[ang]), ny = -dd[ring], nz = sinf(aa[ang]) * zsquash;
                    int bi;

                    for (bi = 0; bi < 2; bi++) {          /* 两个乳房 */
                        float dx = lx - g_bulge[bi].c.x;
                        float dy = ly - g_bulge[bi].c.y;
                        float dz = lz - g_bulge[bi].c.z;
                        float dd2 = sqrtf(dx * dx + dy * dy + dz * dz);
                        float w = 1.0f - dd2 / g_bulge[bi].R;

                        if (w > 0.0f) {
                            w = w * w * (3.0f - 2.0f * w);
                            lx += g_bulge[bi].dir.x * g_bulge[bi].amp * w;
                            ly += g_bulge[bi].dir.y * g_bulge[bi].amp * w;
                            lz += g_bulge[bi].dir.z * g_bulge[bi].amp * w;
                            nx += g_bulge[bi].dir.x * w * 1.2f;
                            ny += g_bulge[bi].dir.y * w * 1.2f;
                            nz += g_bulge[bi].dir.z * w * 1.2f;
                        }
                    }
                    q[k] = W(lx, ly, lz);
                    nv = XMVector3Normalize(XMVectorSet(nx, ny, nz, 0));
                }
                XMStoreFloat3(&nn[k], nv);
                /* 法线跟着她的朝向转 */
                {
                    float sn = sinf(g_wm_yaw), cs = cosf(g_wm_yaw);
                    XMFLOAT3 t2;

                    t2.x = nn[k].x * cs + nn[k].z * sn;
                    t2.z = -nn[k].x * sn + nn[k].z * cs;
                    t2.y = nn[k].y;
                    nn[k] = t2;
                }
                g_aux.x = aux_lo + (aux_hi - aux_lo) * tt[ring];   /* 呼吸幅度：下面小、上面大 */
            }
            Push(q[0], nn[0], col); Push(q[1], nn[1], col); Push(q[2], nn[2], col);
            Push(q[0], nn[0], col); Push(q[2], nn[2], col); Push(q[3], nn[3], col);
        }
    }
    g_aux = { 0.0f, 0.0f };
}

/* 一根毛：从 p0 出发往外岔的一小片（两个三角形，正反两面都有），
 * 所以头发的边缘、胯下那片都是碎的，不是一块塑料壳。
 * p0 / lea / up 全都是"她自己身上的"局部坐标和局部角度，
 * 由里面的 W() 负责摆到世界上去 —— 这样躺下、坐下都跟着身体走。 */
static void AddHairStrand(XMFLOAT3 p0, float lea, float up, float len, float bow,
                          float w, XMFLOAT3 col)
{
    XMFLOAT3 v1, v2, mid;
    XMFLOAT3 d1 = { sinf(lea) * cosf(up), sinf(up), cosf(lea) * cosf(up) };
    XMFLOAT3 d2 = { sinf(lea + bow * 0.7f), up - 0.5f, cosf(lea + bow * 0.7f) };
    XMFLOAT3 S, U;
    XMVECTOR n, t;

    mid.x = p0.x + d1.x * len * 0.5f;
    mid.y = p0.y + d1.y * len * 0.5f;
    mid.z = p0.z + d1.z * len * 0.5f;
    v1.x = mid.x + d2.x * len * 0.5f;
    v1.y = mid.y + d2.y * len * 0.5f;
    v1.z = mid.z + d2.z * len * 0.5f;
    v2.x = p0.x + (v1.x - p0.x) * 0.5f;
    v2.y = p0.y + (v1.y - p0.y) * 0.5f;
    v2.z = p0.z + (v1.z - p0.z) * 0.5f;

    n = XMVector3Normalize(XMVectorSet(sinf(lea), 0.30f, cosf(lea), 0));
    t = XMVector3Normalize(XMVector3Cross(n, XMVectorSet(0, 1, 0, 0)));
    XMStoreFloat3(&S, t * (w * 0.5f));
    XMStoreFloat3(&U, n * (w * 0.8f));

    {
        XMFLOAT3 q0 = { p0.x - S.x, p0.y - S.y, p0.z - S.z };
        XMFLOAT3 q1 = { p0.x + S.x, p0.y + S.y, p0.z + S.z };
        XMFLOAT3 q2 = { v2.x + S.x * 0.6f, v2.y + S.y * 0.6f, v2.z + S.z * 0.6f };
        XMFLOAT3 q3 = { v2.x - S.x * 0.6f, v2.y - S.y * 0.6f, v2.z - S.z * 0.6f };
        XMFLOAT3 q4 = { v1.x + S.x * 0.16f, v1.y + S.y * 0.16f, v1.z + S.z * 0.16f };
        XMFLOAT3 q5 = { v1.x - S.x * 0.16f, v1.y - S.y * 0.16f, v1.z - S.z * 0.16f };
        XMFLOAT3 nn;

        nn.x = XMVectorGetX(n);
        nn.y = XMVectorGetY(n);
        nn.z = XMVectorGetZ(n);
        (void)U;

        Push(W(q0.x, q0.y, q0.z), nn, col); Push(W(q1.x, q1.y, q1.z), nn, col);
        Push(W(q2.x, q2.y, q2.z), nn, col);
        Push(W(q0.x, q0.y, q0.z), nn, col); Push(W(q2.x, q2.y, q2.z), nn, col);
        Push(W(q3.x, q3.y, q3.z), nn, col);

        /* 上半段：收成一个尖 */
        Push(W(q2.x, q2.y, q2.z), nn, col); Push(W(q4.x, q4.y, q4.z), nn, col);
        Push(W(q5.x, q5.y, q5.z), nn, col);
        Push(W(q2.x, q2.y, q2.z), nn, col); Push(W(q5.x, q5.y, q5.z), nn, col);
        Push(W(q3.x, q3.y, q3.z), nn, col);
    }
}

/* 一圈毛：在 (cx,cy,cz) 附近按半径 rr 撒 n 根（rnd 可以给不同随机序列） */
static void AddHairTuft(float cx, float cy, float cz, float rr, int n,
                        float len0, float len1, float w, XMFLOAT3 col, int seed)
{
    int i;

    srand(seed);
    for (i = 0; i < n; i++) {
        float a = (float)rand() / RAND_MAX * 6.2831853f;
        float rad = rr * (0.25f + 0.75f * sqrtf((float)rand() / RAND_MAX));
        float bx = cx + sinf(a) * rad;
        float bz = cz + cosf(a) * rad;
        float by = cy + ((float)rand() / RAND_MAX - 0.5f) * rr * 2.2f;
        float len = len0 + (float)rand() / RAND_MAX * (len1 - len0);
        float down = -0.55f - (float)rand() / RAND_MAX * 1.05f;
        float dk = 0.55f + (float)rand() / RAND_MAX * 0.8f;
        XMFLOAT3 c2 = { col.x * dk, col.y * dk, col.z * dk };

        AddHairStrand(V3(bx, by, bz), a, down, len,
                      ((float)rand() / RAND_MAX - 0.5f) * 0.7f,
                      w * (0.7f + (float)rand() / RAND_MAX * 0.7f), c2);
    }
}

/* 一大把毛：从 a 出发，顺着 d 的方向扫出去，越往下越往外散。
 * 头上的长发、马尾都用它 —— 一根根摞起来，边上是碎的，不是一根塑料管。 */
static void AddHairSweep(XMFLOAT3 a, XMFLOAT3 d, float len, float spread, float curl,
                         float w, XMFLOAT3 col, int n, int seed)
{
    int k;

    /* a 和 d 都是"她自己身上"的局部坐标（发根、生长方向），
     * 每根毛最后由 AddHairStrand 里的 W() 摆到世界上，
     * 所以躺下、坐下的时候头发会跟着身体一起走。 */
    srand(seed);
    for (k = 0; k < n; k++) {
        XMFLOAT3 dir = d;
        float l = len;
        float roll = (float)rand() / RAND_MAX * 6.2831853f;
        float jitter = (0.004f + 0.022f * (float)rand() / RAND_MAX) * spread;

        dir.x += cosf(roll) * jitter;
        dir.z += sinf(roll) * jitter;
        dir.y += ((float)rand() / RAND_MAX - 0.5f) * 0.22f;
        l *= 0.85f + (float)rand() / RAND_MAX * 0.30f;
        {
            float dk = 0.70f + (float)rand() / RAND_MAX * 0.60f;
            XMFLOAT3 c2 = { col.x * dk, col.y * dk, col.z * dk };
            float lea = atan2f(dir.x, dir.z) + curl * 0.16f;
            float dn = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z) + 0.0001f;
            float pitch = asinf(dir.y / dn);

            AddHairStrand(a, lea, pitch, l, curl * 0.5f,
                          w * (0.75f + (float)rand() / RAND_MAX * 0.6f), c2);
        }
    }
}

/* 躺姿兜底：转过来以后量一遍最低点，整块抬起来，让背和后脑贴着地 */
static float LieLiftFix(void)
{
    UINT i2;
    float mn = 1e9f, lift;

    if (g_vn == 0)
        return 0.0f;
    for (i2 = 0; i2 < g_vn; i2++)
        if (g_vbuf_a[i2].pos.y < mn)
            mn = g_vbuf_a[i2].pos.y;
    lift = 0.015f - mn;
    for (i2 = 0; i2 < g_vn; i2++)
        g_vbuf_a[i2].pos.y += lift;
    return lift;
}

/* 坐姿兜底：量一下最低的顶点，把整块抬起来，保证正好坐在地上。
 * 关节是算出来的，脚踝、脚掌这些圆球的半径总会多出几厘米，
 * 与其一个个去凑半径，不如整体抬 —— 一定不会穿地，也不会陷进去。 */
static float SitGroundFix(void)
{
    UINT i2;
    float mn = 1e9f, lift = 0.0f;

    if (g_vn == 0)
        return 0.0f;
    for (i2 = 0; i2 < g_vn; i2++)
        if (g_vbuf_a[i2].pos.y < mn)
            mn = g_vbuf_a[i2].pos.y;
    lift = (g_wm_at.y + 0.005f) - mn;
    if (lift > 0.0005f || lift < -0.0005f)
        for (i2 = 0; i2 < g_vn; i2++)
            g_vbuf_a[i2].pos.y += lift;
    return lift;
}

/* 地图上随机生成的一个女性角色：圆润版——四肢是圆管，头/胸/关节是球。
 * 局部坐标以她脚下为原点、面朝 +Z；骨骼朝向由 yaw 决定。
 * 她的颜色比"真实"稍亮一点，背光的那一面也看得清。 */
static void BuildWoman(XMFLOAT3 at, float yaw)
{
    XMFLOAT3 skin  = { 0.97f, 0.84f, 0.74f };
    XMFLOAT3 skin2 = { 0.93f, 0.79f, 0.69f };
    XMFLOAT3 hair  = { 0.42f, 0.27f, 0.18f };
    XMFLOAT3 white = { 0.99f, 0.99f, 0.97f };
    XMFLOAT3 dark  = { 0.12f, 0.11f, 0.11f };
    XMFLOAT3 lip   = { 0.78f, 0.36f, 0.34f };
    XMFLOAT3 up    = { 0, 1, 0 };
    int sd;

    g_wm_at = at;
    g_wm_yaw = yaw;

    /* 关键：清空顶点表，从头攒她自己的顶点。
     * 少了这一句，缓冲里前面装的是上一次攒的"手 + 右上角 FPS 数字"
     * （它们就摆在镜头前 0.45 米处），那些人一画就先把镜头糊死，
     * 后面的身体全被深度遮挡——看着就像"她根本没被画出来"。 */
    g_vn = 0;

    if (g_pose == POSE_LIE) {
        /* ---- 仰面躺着、两条腿弯起来（膝盖朝上）----
         * 全用"站姿局部坐标"写：y 是身高方向（头顶那一边），z 是身前方向。
         * 躺着就是整块绕 X 轴转 90°（见 Xform），所以：
         *   局部 y → 世界 z（头顶朝前），局部 z → 世界上方（脸朝上）。
         * 关节：髋 (±0.082, 0.925, 0) → 膝 (±0.130, 0.480, 0.190) → 脚 (±0.150, 0.080, 0.330)
         * 大腿 0.47、小腿 0.44，和站姿一致；转过来膝盖在世界 0.38 米高、脚踩回地上。 */
        XMFLOAT3 sidev = { cosf(yaw), 0, -sinf(yaw) };        /* 她自己的左右方向 */
        int i;

        g_aux = { 0.55f, 0.0f };
        for (i = -1; i <= 1; i += 2) {
            float hxx = i * 0.082f;                           /* 髋 */
            float kxx = i * 0.130f, kyy = LIE_KNEE, kzz = 0.190f;   /* 膝盖：弯起来 */
            float axx = i * 0.150f, ayy = LIE_FOOT, azz = 0.330f;   /* 脚：踩回地上 */
            XMFLOAT3 hp, k3, a3, f3;

            hp = W(hxx, 0.925f, 0.0f);
            k3 = W(kxx, kyy, kzz);
            a3 = W(axx, ayy, azz);
            f3 = W(axx, ayy, azz + 0.060f);

            AddBall(hp, 0.086f, 0.088f, 0.086f, skin);                        /* 髋 */
            AddTube(hp, k3, 0.080f, 0.058f, sidev, skin);                     /* 大腿 */
            AddBall(k3, 0.058f, 0.058f, 0.058f, skin);                       /* 膝盖 */
            AddTube(k3, a3, 0.058f, 0.042f, sidev, skin);                    /* 小腿 */
            AddBall(f3, 0.046f, 0.038f, 0.085f, skin2);                      /* 脚 */
        }
    } else if (g_pose == POSE_SIT) {
        /* ---- 坐在地上、两条腿朝前岔开、膝盖架起来 ----
         * 每个数字都是被约束逼出来的，不是拍的：
         *   ① 屁股底面贴地（0.03 米）→ 胯中 0.115 米高；
         *   ② 脚要落在 0.22 米外（收在身前，不能把腿伸直摊一地），
         *      而大腿 0.46 + 小腿 0.44 = 0.90 米必须刚好用掉，
         *      所以膝盖只能架到 0.50 米高、伸到 0.35 米外；
         *   ③ 手放身侧地上：肩 0.65 米，胳膊 0.44 米，刚好够到地面。
         * 大腿用"侧向"当参考轴，岔得再开侧面也不会翻。 */
        XMFLOAT3 sidev = { cosf(yaw), 0, -sinf(yaw) };        /* 她自己的左右方向 */
        XMFLOAT3 fw    = { sinf(yaw), 0,  cosf(yaw) };        /* 她面朝的方向 */
        int i;

        g_aux = { 0.55f, 0.0f };
        for (i = -1; i <= 1; i += 2) {
            float hx = i * 0.082f;                            /* 髋：跟着胯走 */
            float kx = i * 0.320f, ky = 0.340f, kz = 0.340f;  /* 膝盖：岔开、架起来 */
            float ax = i * 0.305f, ay = 0.055f, az = 0.500f;  /* 脚：踩在身前地上 */
            XMFLOAT3 hp, k3, a3, f3;

            hp = hip3(hx, 0.925f + SitDrop(0.925f), 0.0f);    /* 髋（跟着下沉，不跟着弯腰） */
            k3 = W(kx, ky, kz);
            a3 = W(ax, ay, az);
            f3 = W(ax + fw.x * 0.060f, ay, az + fw.z * 0.060f);

            AddBall(hp, 0.086f, 0.088f, 0.086f, skin);                        /* 髋 */
            AddTube(hp, k3, 0.080f, 0.058f, sidev, skin);                     /* 大腿 */
            AddBall(k3, 0.058f, 0.058f, 0.058f, skin);                       /* 膝盖 */
            AddTube(k3, a3, 0.058f, 0.042f, sidev, skin);                    /* 小腿 */
            AddBall(f3, 0.046f, 0.038f, 0.085f, skin2);                      /* 脚 */
        }
    } else {
        /* ---- 站着：腿：圆管 + 膝盖球 + 脚 ---- */
        g_aux = { 0.55f, 0.0f };
        for (sd = -1; sd <= 1; sd += 2) {
            float sx = sd * 0.082f;

            /* 髋关节的球：把胯和大腿接顺，不会有台阶 */
            AddBall(W(sx, 0.925f, 0.0f), 0.086f, 0.088f, 0.086f, skin);
            AddTube(W(sx, 0.945f, 0), W(sx, 0.50f, 0.01f), 0.080f, 0.058f, up, skin);
            AddBall(W(sx, 0.50f, 0.01f), 0.058f, 0.058f, 0.058f, skin);
            AddTube(W(sx, 0.50f, 0.01f), W(sx, 0.11f, 0.0f), 0.058f, 0.042f, up, skin);
            AddBall(W(sx, 0.055f, 0.055f), 0.046f, 0.038f, 0.085f, skin2);
        }
    }

    /* ---- 胳膊：肩球 + 两段圆管 + 手 ---- */
    for (sd = -1; sd <= 1; sd += 2) {
        XMFLOAT3 sh, el, wr;

        g_aux = { 0.30f, 0.0f };
        AddBall(W(sd * 0.135f, 1.345f, 0), 0.055f, 0.055f, 0.055f, skin);
        sh = W(sd * 0.135f, 1.345f, 0);
        if (g_pose == POSE_SIT) {
            /* 坐姿：胳膊垂下去、手撑在身侧的地上（膝盖岔开，手不会碰到腿）。
             * 压缩只作用在腰腹，肩膀仍在 0.50 米，胳膊 0.44 米，正好够到地面。 */
            XMFLOAT3 tgt = W(sd * 0.300f, 0.030f, -0.140f);   /* 身侧略偏后的地面 */
            float    ux = tgt.x - sh.x, uy = tgt.y - sh.y, uz = tgt.z - sh.z;
            float    ul = sqrtf(ux * ux + uy * uy + uz * uz) + 0.0001f;
            float    l1 = 0.230f, l2 = 0.210f;

            el.x = sh.x + ux / ul * l1;                       /* 肘：上臂长度处 */
            el.y = sh.y + uy / ul * l1;
            el.z = sh.z + uz / ul * l1;
            if (ul <= l1 + l2) {                              /* 够得到：手腕落在目标上 */
                wr = tgt;
            } else {                                          /* 够不到：胳膊伸直，朝目标 */
                wr.x = el.x + ux / ul * l2;
                wr.y = el.y + uy / ul * l2;
                wr.z = el.z + uz / ul * l2;
            }
        } else if (g_pose == POSE_LIE) {
            /* 躺姿：胳膊顺着身体摊在两侧地上（局部 z 就是"贴地"的方向）。
             * 肩在 (0.135, 1.345, 0)，肘往"往下、往外"，手落在胯旁边的地上。 */
            el = W(sd * 0.165f, 1.115f, 0.020f);
            wr = W(sd * 0.190f, 0.895f, 0.035f);
        } else {
            el = W(sd * 0.168f, 1.115f, 0.02f);
            wr = W(sd * 0.178f, 0.895f, 0.035f);
        }
        AddTube(sh, el, 0.048f, 0.040f, up, skin);
        AddBall(el, 0.040f, 0.040f, 0.040f, skin);
        AddTube(el, wr, 0.040f, 0.032f, up, skin);
        AddBall(wr, 0.035f, 0.030f, 0.045f, skin2);
        g_aux = { 0.0f, 0.0f };
    }

    /* ---- 胯 / 腰 / 肚子 / 胸 / 肩：一条连续轮廓转出来的整块身体 ---- */
    {
        static const float prof[][2] = {           /* 高度, 横向半径 */
            { 0.855f, 0.104f }, { 0.885f, 0.126f }, { 0.930f, 0.134f },
            { 0.975f, 0.128f }, { 1.020f, 0.116f }, { 1.065f, 0.106f },
            { 1.105f, 0.103f }, { 1.150f, 0.108f }, { 1.195f, 0.118f },
            { 1.240f, 0.131f }, { 1.280f, 0.136f }, { 1.320f, 0.132f },
            { 1.360f, 0.121f }, { 1.400f, 0.100f },
            { 1.432f, 0.074f }, { 1.455f, 0.058f },        /* 脖子：细下去 */
            { 1.490f, 0.064f }, { 1.520f, 0.078f },        /* 下颌 */
            { 1.560f, 0.090f }, { 1.600f, 0.088f },        /* 头最宽处 */
            { 1.635f, 0.070f }, { 1.660f, 0.040f }, { 1.672f, 0.004f }
        };

        AddLathe(prof, (int)(sizeof(prof) / sizeof(prof[0])), 0.72f, 20, 0.10f, 1.0f, skin);
    }

    /* ---- 乳头 + 乳晕：贴在鼓出来的那两个尖端上 ---- */
    g_aux = { 0.90f, 0.0f };
    {
        XMFLOAT3 nip = { 0.66f, 0.38f, 0.36f };        /* 奶头的颜色 */
        XMFLOAT3 are = { 0.76f, 0.50f, 0.45f };        /* 乳晕：浅一圈 */
        int side;

        for (side = -1; side <= 1; side += 2) {
            AddBall(W(side * 0.092f, 1.252f, 0.158f), 0.027f, 0.024f, 0.018f, are);
            AddBall(W(side * 0.096f, 1.256f, 0.166f), 0.010f, 0.010f, 0.009f, nip);
        }
    }

    /* ---- 阴毛：胯下那片，一根一根往外岔的真毛 ---- */
    g_aux = { 0.10f, 0.0f };
    {
        XMFLOAT3 pub = { 0.34f, 0.21f, 0.14f };
        int i;

        /* 从下往上铺成一个倒三角：下面窄、上面宽，每根都往外岔 */
        for (i = 1; i < 6; i++) {
            float t = (float)i / 5.0f;
            float y = 0.892f + t * 0.108f;
            float half = 0.052f * (0.35f + 0.65f * (1.0f - t));

            AddHairTuft(0.0f, y, 0.058f + t * 0.008f, half, 44,
                        0.022f, 0.038f, 0.0062f, pub, 900 + i);
        }
    }

    /* ---- 眼睛 + 嘴 ---- */
    {
        XMFLOAT3 iris  = { 0.32f, 0.22f, 0.14f };      /* 虹膜 */
        XMFLOAT3 brow  = { 0.28f, 0.18f, 0.13f };      /* 眉毛 */
        int side;

        for (side = -1; side <= 1; side += 2) {
            float ex = side * 0.034f;

            AddBall(W(ex, 1.588f, 0.052f), 0.0205f, 0.0225f, 0.014f, white);   /* 眼白 */
            AddBall(W(ex, 1.587f, 0.063f), 0.0115f, 0.0125f, 0.009f, iris);    /* 虹膜 */
            AddBall(W(ex, 1.587f, 0.069f), 0.0055f, 0.0060f, 0.005f, dark);    /* 瞳孔 */
            AddBall(W(ex, 1.6005f, 0.050f), 0.0215f, 0.008f, 0.014f, skin2);   /* 上眼睑 */
            AddBall(W(ex, 1.5755f, 0.050f), 0.0205f, 0.006f, 0.013f, skin2);   /* 下眼睑 */
            AddBall(W(ex, 1.616f, 0.046f), 0.024f, 0.0045f, 0.010f, brow);     /* 眉毛 */
        }
        /* 鼻子：鼻梁 + 鼻头 + 两个鼻孔 */
        AddTube(W(0, 1.560f, 0.052f), W(0, 1.532f, 0.076f), 0.010f, 0.017f, up, skin);
        AddBall(W(0, 1.526f, 0.078f), 0.019f, 0.016f, 0.019f, skin);
        AddBall(W(0.010f, 1.521f, 0.083f), 0.005f, 0.004f, 0.005f, dark);
        AddBall(W(-0.010f, 1.521f, 0.083f), 0.005f, 0.004f, 0.005f, dark);
        /* 嘴：上下唇各一条，中间留一条缝 */
        AddBall(W(0, 1.5035f, 0.066f), 0.024f, 0.0055f, 0.011f, lip);
        AddBall(W(0, 1.4955f, 0.066f), 0.023f, 0.0060f, 0.011f, lip);
    }

    /* ---- 头发：全都在后脑和后面 ----
     * 头皮那层壳只盖住后半边，前面的额头、脸一点都不盖；
     * 所有发根也都在"后半圈"上（a 落在 [2.35, 3.89]，也就是 x≈0 偏后的那一段），
     * 没有刘海、没有鬓角，一眼看过去头发全在后面。 */
    {
        XMFLOAT3 hc = { 0, 1.598f, -0.006f };      /* 头皮的球心往后挪一点 */
        float hrx = 0.092f, hry = 0.094f, hrz = 0.098f;
        int i, j;

        g_aux = { 0.18f, 0.20f };
        {
            /* 只画后半边的壳：纬度从头顶往下、经度只取后半圈，前面一点不盖 */
            const int ROWS = 7, SEG = 9;
            const float ph0 = 0.14f, ph1 = 1.62f;          /* 头顶 → 后颈 */
            const float th0 = 2.30f, th1 = 3.98f;          /* 后半圈（以 -Z 为中心） */

            for (i = 0; i < ROWS; i++) {
                float p0 = ph0 + (ph1 - ph0) * (float)i / ROWS;
                float p1 = ph0 + (ph1 - ph0) * (float)(i + 1) / ROWS;

                for (j = 0; j < SEG; j++) {
                    float t0 = th0 + (th1 - th0) * (float)j / SEG;
                    float t1 = th0 + (th1 - th0) * (float)(j + 1) / SEG;
                    XMFLOAT3 p[4], nn[4];
                    float pp[2] = { p0, p1 };
                    float tt[2] = { t0, t1 };
                    int k;

                    for (k = 0; k < 4; k++) {
                        int ri = (k >= 2) ? 1 : 0;
                        int ci = (k == 1 || k == 2) ? 1 : 0;
                        XMFLOAT3 nrm = { sinf(pp[ri]) * sinf(tt[ci]), cosf(pp[ri]),
                                         sinf(pp[ri]) * cosf(tt[ci]) };

                        nn[k] = nrm;
                        p[k].x = hc.x + nrm.x * hrx;
                        p[k].y = hc.y + nrm.y * hry;
                        p[k].z = hc.z + nrm.z * hrz;
                    }
                    Push(W(p[0].x, p[0].y, p[0].z), nn[0], hair);
                    Push(W(p[1].x, p[1].y, p[1].z), nn[1], hair);
                    Push(W(p[2].x, p[2].y, p[2].z), nn[2], hair);
                    Push(W(p[0].x, p[0].y, p[0].z), nn[0], hair);
                    Push(W(p[2].x, p[2].y, p[2].z), nn[2], hair);
                    Push(W(p[3].x, p[3].y, p[3].z), nn[3], hair);
                }
            }
        }

        /* 一根一根的长发：发根全在后脑，往下、往后淌 */
        g_aux = { 0.35f, 1.0f };
        {
            float sit = (g_pose == POSE_SIT) ? 0.72f : 1.0f;   /* 坐着的时候头发短一点，不然拖地上 */

            for (i = 0; i < 6; i++) {
                float y = 1.588f - i * 0.024f;
                float ex = 0.080f + i * 0.006f;
                float ez = 0.072f + i * 0.006f;
                float len = (0.10f + i * 0.028f) * sit;

                for (j = 0; j < 10; j++) {
                    float a = 2.35f + j * (1.54f / 9.0f);      /* 后半圈，不绕到前面 */
                    XMFLOAT3 base = { sinf(a) * ex, y, -0.014f - cosf(a) * ez };
                    XMFLOAT3 dir = { sinf(a) * 0.24f, -1.0f, -0.20f - cosf(a) * 0.28f };

                    AddHairSweep(V3(base.x, base.y, base.z), dir,
                                 len * (0.92f + 0.16f * (float)((i + j) % 3)),
                                 (i < 2) ? 0.35f : 0.60f,
                                 ((float)((i + j) % 3) - 1.0f) * 0.5f,
                                 0.0080f, hair, 12, 4000 + i * 13 + j * 3);
                }
            }
            /* 头顶往后倒的那一层：也全在后半圈上，不在前面 */
            AddHairSweep(V3(0.020f, 1.650f, -0.030f), V3(0.12f, -0.55f, -0.85f),
                         0.13f * sit, 1.0f, 0.4f, 0.0075f, hair, 30, 777);
            AddHairSweep(V3(-0.035f, 1.646f, -0.040f), V3(-0.22f, -0.60f, -0.80f),
                         0.12f * sit, 1.0f, -0.5f, 0.0075f, hair, 26, 778);
            AddHairSweep(V3(0.0f, 1.620f, -0.062f), V3(0.0f, -0.75f, -0.60f),
                         0.11f * sit, 1.0f, 0.0f, 0.0070f, hair, 22, 781);
        }
    }
    g_aux = { 0.0f, 0.0f };
}

/* ---------------- 着色器 ---------------- */
/* 天空和太阳都在着色器里算：天顶深蓝、地平线发白的一条渐变，
 * 太阳是一个亮盘 + 三层光晕，越靠近太阳天越亮。
 * 水和天空用同一个 SkyCol()，所以水面反出来的就是天上那块颜色。 */
static const char *g_sky_hlsl =
"cbuffer CB : register(b0) { row_major float4x4 mvp; float4 light; float4 anim; float4 eye; };\n"
"struct SkyIn  { float3 ndc : POSITION; float3 ray : TEXCOORD0; };\n"
"struct SkyOut { float4 p : SV_POSITION; float3 ray : TEXCOORD0; };\n"
"float3 SunDir(void) { return normalize(-light.xyz); }\n"
"float3 SkyCol(float3 d) {\n"
"    float up = saturate(d.y);\n"
"    float3 zen = float3(0.20f, 0.42f, 0.85f);\n"          /* 天顶：深蓝 */
"    float3 hor = float3(0.70f, 0.84f, 0.96f);\n"          /* 地平线：发白（雾色也是它） */
"    float3 c = lerp(hor, zen, pow(up, 0.55f));\n"
"    float sd = saturate(dot(d, SunDir()));\n"
"    c += float3(1.0f, 0.93f, 0.78f) * pow(sd, 6.0f) * 0.30f;\n"   /* 太阳附近天更亮 */
"    return c;\n"
"}\n"
"SkyOut SkyVS(SkyIn i) {\n"
"    SkyOut o;\n"
"    o.p = float4(i.ndc.xy, 0.5f, 1.0f);\n"
"    o.ray = i.ray;\n"
"    return o;\n"
"}\n"
"float4 SkyPS(SkyOut i) : SV_Target {\n"
"    float3 d = normalize(i.ray);\n"
"    float3 c = SkyCol(d);\n"
"    float cd = dot(d, SunDir());\n"
"    float disc = smoothstep(0.99960f, 0.99985f, cd);\n"   /* 太阳本体（约 1.2 度） */
"    float glow = pow(saturate(cd), 2600.0f) * 0.85f\n"    /* 紧贴着的一圈 */
"               + pow(saturate(cd), 130.0f) * 0.20f\n"     /* 中等光晕 */
"               + pow(saturate(cd), 9.0f) * 0.07f;\n"      /* 大范围的天光 */
"    c += float3(1.0f, 0.97f, 0.90f) * disc * 1.8f;\n"
"    c += float3(1.0f, 0.94f, 0.80f) * glow;\n"
"    return float4(c, 1.0f);\n"
"}\n";

static const char *g_hlsl =
"cbuffer CB : register(b0) { row_major float4x4 mvp; float4 light; float4 anim; float4 eye; };\n"
"struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; float2 aux : TEXCOORD0; };\n"
"struct VSOut { float4 p : SV_POSITION; float3 n : NORMAL; float3 c : COLOR; float emiss : TEXCOORD1; float d : TEXCOORD2; float auxy : TEXCOORD3; float3 wp : TEXCOORD4; };\n"
"float3 SunDir(void) { return normalize(-light.xyz); }\n"
"float3 SkyCol(float3 d) {\n"
"    float up = saturate(d.y);\n"
"    float3 zen = float3(0.20f, 0.42f, 0.85f);\n"
"    float3 hor = float3(0.70f, 0.84f, 0.96f);\n"
"    float3 c = lerp(hor, zen, pow(up, 0.55f));\n"
"    float sd = saturate(dot(d, SunDir()));\n"
"    c += float3(1.0f, 0.93f, 0.78f) * pow(sd, 6.0f) * 0.30f;\n"
"    return c;\n"
"}\n"
"VSOut VSMain(VSIn i) {\n"
"    VSOut o;\n"
"    float3 p = i.pos;\n"
"    float t = anim.x;\n"
"    if (i.aux.x > 0.0f) {                      /* 呼吸：胸口和头上下轻轻起伏 */\n"
"        p.y += sin(t * 1.05f) * 0.014f * i.aux.x;\n"
"        p.z += cos(t * 1.05f) * 0.005f * i.aux.x;\n"
"    }\n"
"    if (i.aux.y > 0.0f && i.aux.x > -2.5f) {   /* 头发：一层一层飘（水面不算，它有别的用处） */\n"
"        float w = sin(t * 1.15f + p.y * 3.1f) * 0.034f + sin(t * 0.55f + p.x * 5.0f) * 0.020f;\n"
"        p.x += w * i.aux.y;\n"
"        p.z += (w * 0.7f + sin(t * 0.85f) * 0.014f) * i.aux.y;\n"
"        p.y -= 0.004f * i.aux.y * sin(t * 1.15f);\n"
"    }\n"
"    o.p = mul(float4(p, 1.0f), mvp);\n"
"    o.n = i.nrm;\n"
"    o.c = i.col;\n    o.emiss = i.aux.x;\n"
"    o.auxy = i.aux.y;\n"
"    o.wp = p;\n"
"    o.d = length(p - eye.xyz);                 /* 离镜头多远：算雾用 */\n"
"    return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_Target {\n"
"    float3 L = SunDir();\n"
"    float  d = saturate(dot(normalize(i.n), L));\n"
"    float  amb = light.w;                                     /* 每个物体可以不一样 */\n"
"    float  k = (i.emiss < 0.0f) ? 1.0f : (amb + (1.0f - amb) * d);\n"
"    float3 c = i.c * k;\n"
"    float  a = (i.emiss < -2.5f) ? i.auxy : anim.w;           /* 水面按顶点给透明度 */\n"
"    float  fog = saturate((i.d - anim.y) / max(anim.z - anim.y, 1.0f));\n"
"    if (i.emiss < -2.5f) {                    /* ---- 水面：清 + 反光 ---- */\n"
"        float t = anim.x;\n"
"        float2 q = i.wp.xz;\n"
"        /* 微波：法线轻轻晃，反光就碎成一片，不是死板的一面镜子 */\n"
"        float3 N = normalize(float3(sin(q.x * 1.7f + t * 1.1f) * 0.035f + sin(q.x * 0.7f - t * 0.7f) * 0.022f,\n"
"                                    1.0f,\n"
"                                    cos(q.y * 1.9f + t * 0.9f) * 0.035f + cos(q.y * 0.6f + t * 1.3f) * 0.022f));\n"
"        float3 V = normalize(eye.xyz - i.wp);                 /* 从水面看镜头 */\n"
"        float3 R = reflect(-V, N);\n"
"        float fres = 0.02f + 0.88f * pow(1.0f - saturate(dot(N, V)), 5.0f);\n"
"        float3 refl = SkyCol(R);\n"
"        float spec = pow(saturate(dot(R, L)), 350.0f) * 3.2f;  /* 太阳在水面上的碎光 */\n"
"        c = lerp(c, refl, fres);                              /* 越平视越像镜子 */\n"
"        c += float3(1.0f, 0.97f, 0.90f) * spec;\n"
"        a = a + (1.0f - a) * fres;                            /* 反光强的地方就不透明了 */\n"
"        c = lerp(c, float3(0.70f, 0.84f, 0.96f), fog);\n"
"    } else if (i.emiss >= 0.0f) {              /* 雾：远处慢慢化进天空色，看不到地图的边 */\n"
"        c = lerp(c, float3(0.70f, 0.84f, 0.96f), fog);\n"
"    }\n"
"    return float4(c, a);\n"
"}\n";

static int CompileShaders(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr;
    D3D11_INPUT_ELEMENT_DESC lay[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    D3D11_INPUT_ELEMENT_DESC skylay[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = D3DCompile(g_hlsl, strlen(g_hlsl), NULL, NULL, NULL, "VSMain", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        if (err) {
            MultiByteToWideChar(CP_UTF8, 0, (const char *)err->GetBufferPointer(), -1, g_errmsg, 900);
            err->Release();
        } else {
            wsprintfW(g_errmsg, L"顶点着色器编译失败 hr=0x%08X", (unsigned)hr);
        }
        return 0;
    }
    hr = D3DCompile(g_hlsl, strlen(g_hlsl), NULL, NULL, NULL, "PSMain", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err) {
            MultiByteToWideChar(CP_UTF8, 0, (const char *)err->GetBufferPointer(), -1, g_errmsg, 900);
            err->Release();
        } else {
            wsprintfW(g_errmsg, L"像素着色器编译失败 hr=0x%08X", (unsigned)hr);
        }
        return 0;
    }

    hr = g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_vs);
    hr = SUCCEEDED(hr) ? g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_ps) : hr;
    hr = SUCCEEDED(hr) ? g_dev->CreateInputLayout(lay, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout) : hr;
    if (FAILED(hr)) {
        wsprintfW(g_errmsg, L"创建着色器/输入布局失败 hr=0x%08X", (unsigned)hr);
        vsb->Release();
        psb->Release();
        return 0;
    }

    vsb->Release();
    psb->Release();

    /* 天空那一遍：自己的 VS/PS + 只吃 POSITION/TEXCOORD 的输入布局 */
    vsb = psb = NULL;
    hr = D3DCompile(g_sky_hlsl, strlen(g_sky_hlsl), NULL, NULL, NULL, "SkyVS", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        if (err) {
            MultiByteToWideChar(CP_UTF8, 0, (const char *)err->GetBufferPointer(), -1, g_errmsg, 900);
            err->Release();
        } else {
            wsprintfW(g_errmsg, L"天空顶点着色器编译失败 hr=0x%08X", (unsigned)hr);
        }
        return 0;
    }
    hr = D3DCompile(g_sky_hlsl, strlen(g_sky_hlsl), NULL, NULL, NULL, "SkyPS", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err) {
            MultiByteToWideChar(CP_UTF8, 0, (const char *)err->GetBufferPointer(), -1, g_errmsg, 900);
            err->Release();
        } else {
            wsprintfW(g_errmsg, L"天空像素着色器编译失败 hr=0x%08X", (unsigned)hr);
        }
        vsb->Release();
        return 0;
    }
    hr = g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_sky_vs);
    hr = SUCCEEDED(hr) ? g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_sky_ps) : hr;
    hr = SUCCEEDED(hr) ? g_dev->CreateInputLayout(skylay, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_sky_layout) : hr;
    vsb->Release();
    psb->Release();
    if (FAILED(hr)) {
        wsprintfW(g_errmsg, L"创建天空着色器失败 hr=0x%08X", (unsigned)hr);
        return 0;
    }

    {   /* 天空：一个铺满屏幕的大三角形，三个顶点上带着"这个像素看出去的方向" */
        D3D11_BUFFER_DESC sbd;

        ZeroMemory(&sbd, sizeof(sbd));
        sbd.Usage = D3D11_USAGE_DYNAMIC;
        sbd.ByteWidth = sizeof(float) * 6 * 3;
        sbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&sbd, NULL, &g_sky_vb)))
            return 0;
    }
    return 1;
}

static int InitD3D(HWND hwnd)
{
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    ID3D11Texture2D *back = NULL;
    D3D11_TEXTURE2D_DESC dd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D11_BUFFER_DESC bd;

    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = WIN_W;
    sd.BufferDesc.Height = WIN_H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd, &g_swap,
                                       &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        /* 显卡这边建不出来（老显卡/驱动/远程桌面），退到 WARP：Windows 自带的软件渲染 */
        if (g_swap) { g_swap->Release(); g_swap = NULL; }
        if (g_dev)  { g_dev->Release();  g_dev = NULL; }
        if (g_ctx)  { g_ctx->Release();  g_ctx = NULL; }
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                                           NULL, 0, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_dev, &fl, &g_ctx);
        if (FAILED(hr)) {
            wsprintfW(g_errmsg, L"创建 D3D11 设备失败（硬件和软件都试过）hr=0x%08X", (unsigned)hr);
            return 0;
        }
    }

    hr = g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    if (FAILED(hr) || !back) {
        wsprintfW(g_errmsg, L"取后备缓冲失败 hr=0x%08X", (unsigned)hr);
        return 0;
    }
    g_dev->CreateRenderTargetView(back, NULL, &g_rtv);
    back->Release();

    back = NULL;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    back->GetDesc(&dd);
    back->Release();
    ZeroMemory(&dd, sizeof(dd));
    dd.Width = WIN_W;
    dd.Height = WIN_H;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    {
        ID3D11Texture2D *depth = NULL;

        if (FAILED(g_dev->CreateTexture2D(&dd, NULL, &depth))) {
            wsprintfW(g_errmsg, L"创建深度缓冲失败");
            return 0;
        }
        ZeroMemory(&dsv, sizeof(dsv));
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g_dev->CreateDepthStencilView(depth, &dsv, &g_dsv);
        depth->Release();
    }

    if (!CompileShaders())
        return 0;

    {   /* 双手是"视图模型"：最后画、不吃深度，永远压在草地上 */
        D3D11_DEPTH_STENCIL_DESC dd2;

        ZeroMemory(&dd2, sizeof(dd2));
        dd2.DepthEnable = TRUE;
        dd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd2.DepthFunc = D3D11_COMPARISON_LESS;
        if (FAILED(g_dev->CreateDepthStencilState(&dd2, &g_ds_on)))
            return 0;
        dd2.DepthEnable = FALSE;
        dd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(g_dev->CreateDepthStencilState(&dd2, &g_ds_off)))
            return 0;
        dd2.DepthEnable = TRUE;                   /* 水面：测深度但不写 */
        if (FAILED(g_dev->CreateDepthStencilState(&dd2, &g_ds_nowrite)))
            return 0;
    }

    {   /* 水面半透明：SrcAlpha / InvSrcAlpha */
        D3D11_BLEND_DESC bd3;

        ZeroMemory(&bd3, sizeof(bd3));
        bd3.RenderTarget[0].BlendEnable = TRUE;
        bd3.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd3.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd3.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd3.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd3.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd3.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd3.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(g_dev->CreateBlendState(&bd3, &g_blend)))
            return 0;
    }

    {   /* 草是单面三角形、双手是方块：关掉背面剔除，免得被剔掉看不见 */
        D3D11_RASTERIZER_DESC rd;

        ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(g_dev->CreateRasterizerState(&rd, &g_rs)))
            return 0;
    }

    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(CBData);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_cb))) {
        wsprintfW(g_errmsg, L"创建常量缓冲失败");
        return 0;
    }

    /* 开局：先把河定下来（她躲河要用），再放她，最后把第一版世界一次攒完 */
    RiverInit();
    PlaceWoman();
    WorldBuildNow(g_eye.x, g_eye.z);

    BuildHud();
    g_hand_n = g_vn;
    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Vtx) * g_hand_n;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_hand_vb)))
        return 0;

    /* woman removed: geometry may still be built, but she is never drawn */
    if (g_pose == POSE_LIE)
        g_sit_lift = LieLiftFix();
    else if (g_pose == POSE_SIT)
        g_sit_lift = SitGroundFix();
    g_woman_n = g_vn;
    {
        UINT i2;
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        FILE *lf = NULL;

        for (i2 = 0; i2 < g_woman_n; i2++) {
            int kk;

            for (kk = 0; kk < 3; kk++) {
                float v = (&g_vbuf_a[i2].pos.x)[kk];

                if (v < mn[kk]) mn[kk] = v;
                if (v > mx[kk]) mx[kk] = v;
            }
        }
        if (fopen_s(&lf, "build\\bbox.txt", "w") == 0 && lf) {
            fprintf(lf, "g_pose=%d dbg=%d verts=%u lift=%.4f\n", g_pose, g_dbg_pose, g_woman_n, g_sit_lift);
            {   /* 直接验一下变换：站姿胯、肩、头顶、胸口分别在世界的哪儿 */
                XMFLOAT3 pa = W(0.0f, 0.925f, 0.0f);
                XMFLOAT3 pb = W(0.0f, 1.345f, 0.0f);
                XMFLOAT3 pc = W(0.0f, 1.672f, 0.0f);
                XMFLOAT3 pd = W(0.0f, 1.598f, 0.22f);

                fprintf(lf, "probe hip=(%.3f,%.3f,%.3f) shoulder=(%.3f,%.3f,%.3f)\n",
                        pa.x, pa.y, pa.z, pb.x, pb.y, pb.z);
                fprintf(lf, "probe headtop=(%.3f,%.3f,%.3f) chest_front=(%.3f,%.3f,%.3f)\n",
                        pc.x, pc.y, pc.z, pd.x, pd.y, pd.z);
            }
            fprintf(lf, "pose=%s  at=(%.2f,%.2f,%.2f) yaw=%.3f\n",
                    (g_pose == POSE_LIE) ? "lie" : ((g_pose == POSE_SIT) ? "sit" : "stand"),
                    g_woman_at.x, g_woman_at.y, g_woman_at.z, g_woman_yaw);
            fprintf(lf, "world bbox: x %.3f..%.3f   y %.3f..%.3f   z %.3f..%.3f\n",
                    mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
            {   /* 高的那些顶点到底是谁：按高度分档计数，顺便记下最高的那个点 */
                UINT hi = 0, mid = 0, lo = 0, i3;
                float best = -1e9f, bx2 = 0, by2 = 0, bz2 = 0;

                for (i3 = 0; i3 < g_woman_n; i3++) {
                    float yy = g_vbuf_a[i3].pos.y;

                    if (yy > 1.0f) hi++;
                    else if (yy > 0.4f) mid++;
                    else lo++;
                    if (yy > best) {
                        best = yy;
                        bx2 = g_vbuf_a[i3].pos.x;
                        by2 = g_vbuf_a[i3].pos.y;
                        bz2 = g_vbuf_a[i3].pos.z;
                    }
                }
                fprintf(lf, "counts: y>1.0 = %u, 0.4<y<=1.0 = %u, y<=0.4 = %u\n", hi, mid, lo);
                fprintf(lf, "highest vertex: (%.3f, %.3f, %.3f)\n", bx2, by2, bz2);
                {
                    /* 把最高的那个顶点反算回她自己的坐标，看是哪一块 */
                    float c = cosf(g_wm_yaw), s = sinf(g_wm_yaw);
                    float vx = bx2 - g_wm_at.x, vz = bz2 - g_wm_at.z;
                    float rlx = vx * c - vz * s, rlz = vx * s + vz * c;

                    fprintf(lf, "  -> after RotY inverse: x=%.3f y=%.3f z=%.3f\n", rlx, by2, rlz);
                    fprintf(lf, "  -> Xform maps (x,y,z) to y'=%.3f+z, z'=z+y ; so local y=%.3f, local z=%.3f\n",
                            LIE_LIFT, rlz, by2 - LIE_LIFT);
                }
            }
            fclose(lf);
        }
    }
    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Vtx) * g_woman_n;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_woman_vb)))
        return 0;

    BuildSelf();
    g_body_n = g_vn;
    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Vtx) * g_body_n;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_body_vb)))
        return 0;

    /* 相机：站在 1.7 米高，朝前看；双手用投影矩阵，所以它们永远跟着镜头 */
    g_view = XMMatrixLookAtLH(XMVectorSet(g_eye.x, g_eye.y, g_eye.z, 1),
                              XMVectorSet(g_eye.x + sinf(g_yaw) * cosf(g_pitch),
                                          g_eye.y + sinf(g_pitch),
                                          g_eye.z + cosf(g_yaw) * cosf(g_pitch), 1),
                              XMVectorSet(0, 1, 0, 0));
    g_proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f),
                                      (float)WIN_W / (float)WIN_H, 0.05f, 400.0f);
    g_cbd.light = XMFLOAT4(-0.30f, -0.62f, -0.72f, 0.0f);   /* 光从太阳那边来 */
    {   wchar_t tb[128];
        wsprintfW(tb, L"fp  world=%u hands=%u", g_world_n, g_hand_n);
        SetWindowTextW(hwnd, tb);
    }
    return 1;
}


/* 每帧的输入：WASD 走，鼠标转视角（光标锁在窗口中间，用位移量转） */
static void UpdateInput(HWND hwnd, float dt)
{
    static int first = 1;
    POINT pt;
    RECT rc;
    float spd = 4.2f;
    float mx = 0.0f, mz = 0.0f;

    GetClientRect(hwnd, &rc);
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    {
        int cx = (rc.right - rc.left) / 2, cy = (rc.bottom - rc.top) / 2;
        int dx = pt.x - cx, dy = pt.y - cy;

        if (first) {
            dx = dy = 0;
            first = 0;
        }
        /* 光标锁在窗口正中：第一帧回中，之后只要动了就回中 */
        if (g_first_frame || dx || dy) {
            POINT c = { cx, cy };

            ClientToScreen(hwnd, &c);
            SetCursorPos(c.x, c.y);
            g_first_frame = 0;
        }
        g_yaw   += dx * 0.0026f;
        g_pitch -= dy * 0.0022f;
        if (g_pitch >  1.35f) g_pitch =  1.35f;
        if (g_pitch < -1.35f) g_pitch = -1.35f;

    }

    g_clock += dt;
    g_fps_now = (g_fps_now > 0.0) ? (g_fps_now * 0.92 + (1.0 / (dt > 0.0 ? dt : 0.016)) * 0.08)
                                  : (1.0 / (dt > 0.0 ? dt : 0.016));

    if (GetAsyncKeyState('W') & 0x8000) { mx += sinf(g_yaw); mz += cosf(g_yaw); }
    if (GetAsyncKeyState('S') & 0x8000) { mx -= sinf(g_yaw); mz -= cosf(g_yaw); }
    if (g_dbg_walk > 0 && g_clock < 1e9) {          /* --walk：调试用的自动前进 */
        static int walked = 0;

        if (walked < g_dbg_walk) {
            mx += sinf(g_yaw);
            mz += cosf(g_yaw);
            walked++;
        }
    }
    if (GetAsyncKeyState('A') & 0x8000) { mx -= cosf(g_yaw); mz += sinf(g_yaw); }
    if (GetAsyncKeyState('D') & 0x8000) { mx += cosf(g_yaw); mz -= sinf(g_yaw); }
    {
        float len = sqrtf(mx * mx + mz * mz);

        if (len > 0.001f) {
            g_eye.x += mx / len * spd * dt;
            g_eye.z += mz / len * spd * dt;
            g_walk_amp += (1.0f - g_walk_amp) * 6.0f * dt;      /* 起步：幅度涨上来 */
        } else {
            g_walk_amp -= g_walk_amp * 6.0f * dt;               /* 停下：慢慢收回去 */
            if (g_walk_amp < 0.001f)
                g_walk_amp = 0.0f;
        }
        g_walk += 7.6f * dt * (0.35f + 0.65f * g_walk_amp);      /* 摆腿的相位 */
        if (g_walk > 6.2831853f)
            g_walk -= 6.2831853f;
    }

    /* 右键：如果正指着她（3.5 米内、偏角不超过 30 度），让她弯腰 */
    g_bend_t -= dt;
    if (g_bend_t < 0.0f)
        g_bend_t = 0.0f;
    if (g_rmb > 0.0f) {
        float dx = g_woman_at.x - g_eye.x, dz = g_woman_at.z - g_eye.z;
        float dist = sqrtf(dx * dx + dz * dz);

        g_rmb = 0.0f;
        if (g_woman_live && dist < 3.5f && dist > 0.05f) {
            float fx = sinf(g_yaw), fz = cosf(g_yaw);

            if ((dx * fx + dz * fz) / dist > 0.86f)
                g_bend_t = 1.9f;               /* 弯一下，约 1.9 秒 */
        }
    }
    if (g_bend_t > 0.0f) {
        float u = 1.0f - g_bend_t / 1.9f;

        g_bend_amt = sinf(u * 3.14159265f);    /* 起来 → 最深 → 直回去 */
    } else {
        g_bend_amt = 0.0f;
    }
    if (g_dbg_bend)                            /* 调试：一直保持弯腰，方便看 */
        g_bend_amt = 1.0f;

    /* 空格：跳（落地才能再跳） */
    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) && g_jy <= 0.0f && g_jvy <= 0.0f)
        g_jvy = 6.8f;
    g_jvy -= 15.5f * dt;
    g_jy  += g_jvy * dt;
    if (g_jy <= 0.0f) {
        g_jy = 0.0f;
        g_jvy = 0.0f;
    }
    if (g_dbg_jy >= 0.0f) {                    /* --jy：跳起来的高度定死，方便截图 */
        g_jy  = g_dbg_jy;
        g_jvy = (g_dbg_jy > 0.0f) ? 1.0f : 0.0f;   /* 当成"还在往上飞"，腿会收起来 */
    }

    /* 撞到她身上就推开：她有一个圆柱形的碰撞箱 */
    if (g_woman_live && g_jy < 0.9f) {
        float dx = g_eye.x - g_woman_at.x;
        float dz = g_eye.z - g_woman_at.z;
        float d2 = dx * dx + dz * dz;
        float rr = WOMAN_R + PLAYER_R;

        if (d2 < rr * rr) {
            float d = sqrtf(d2);

            if (d < 0.0001f) {
                dx = 0.0f; dz = 1.0f; d = 1.0f;
            }
            g_eye.x = g_woman_at.x + dx / d * rr;
            g_eye.z = g_woman_at.z + dz / d * rr;
        }
    }

    /* 树：树干有碰撞箱，撞上去就被推开，穿不过去 */
    TreePushOut();

    /* 呼吸：站着也有，走路时叠上走路的颠簸（跳起来时不再颠）。
     * 脚下地面的高度也加进来：走到河滩上是往下走的。 */
    g_ground_y = GroundH(g_eye.x, g_eye.z);
    g_eye.y = g_ground_y + SELF_EYE + g_jy + sinf((float)g_clock * 1.05f) * 0.011f
                    + ((g_jy > 0.0f) ? 0.0f : fabsf(cosf(g_walk)) * 0.012f * g_walk_amp);

    g_view = XMMatrixLookAtLH(XMVectorSet(g_eye.x, g_eye.y, g_eye.z, 1),
                              XMVectorSet(g_eye.x + sinf(g_yaw) * cosf(g_pitch),
                                          g_eye.y + sinf(g_pitch),
                                          g_eye.z + cosf(g_yaw) * cosf(g_pitch), 1),
                              XMVectorSet(0, 1, 0, 0));
}

/* ---------------- 调试用：日志 + 存一帧图 ----------------
 * 交换链上屏的东西抓屏软件经常抓不到（或者被别的窗口盖住），
 * 所以 --shot 直接把后备缓冲拷出来写成 TGA，所见即所得。 */
static void DbgDump(const char *path)
{
    ID3D11Texture2D *bb = NULL;

    if (!SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) || !bb)
        return;
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_TEXTURE2D_DESC sd;
        ID3D11Texture2D *st = NULL;

        bb->GetDesc(&td);
        ZeroMemory(&sd, sizeof(sd));
        sd.Width = td.Width;
        sd.Height = td.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = td.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(g_dev->CreateTexture2D(&sd, NULL, &st))) {
            FILE *f = NULL;

            g_ctx->CopyResource(st, bb);
            if (fopen_s(&f, path, "wb") == 0 && f) {
                D3D11_MAPPED_SUBRESOURCE ms;

                if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
                    unsigned char hdr[18];
                    unsigned char *row = (unsigned char *)malloc((size_t)td.Width * 4);
                    unsigned x, y;

                    ZeroMemory(hdr, sizeof(hdr));
                    hdr[2] = 2;                                     /* 未压缩真彩 */
                    hdr[12] = (unsigned char)(td.Width & 0xFF);
                    hdr[13] = (unsigned char)(td.Width >> 8);
                    hdr[14] = (unsigned char)(td.Height & 0xFF);
                    hdr[15] = (unsigned char)(td.Height >> 8);
                    hdr[16] = 32;                                   /* BGRA */
                    hdr[17] = 0x28;                                 /* 左上角开始 */
                    fwrite(hdr, 1, sizeof(hdr), f);
                    for (y = 0; y < td.Height; y++) {
                        const unsigned char *src = (const unsigned char *)ms.pData + (size_t)y * ms.RowPitch;

                        for (x = 0; x < td.Width; x++) {
                            row[x * 4 + 0] = src[x * 4 + 0];        /* B */
                            row[x * 4 + 1] = src[x * 4 + 1];        /* G */
                            row[x * 4 + 2] = src[x * 4 + 2];        /* R */
                            row[x * 4 + 3] = 0xFF;                  /* A 丢掉 */
                        }
                        fwrite(row, 1, (size_t)td.Width * 4, f);
                    }
                    if (row)
                        free(row);
                    g_ctx->Unmap(st, 0);
                }
                fclose(f);
            }
            st->Release();
        }
    }
    bb->Release();
}

static void DbgLog(void)
{
    FILE *f = NULL;

    if (fopen_s(&f, g_dbg_log, "w") != 0 || !f)
        return;
    fprintf(f, "world_verts=%u woman_verts=%u hand_verts=%u body_verts=%u\n",
            g_world_n[g_world_cur], g_woman_n, g_hand_n, g_body_n);
    fprintf(f, "world center=(%.2f, %.2f) stage=%d\n", g_wcx, g_wcz, g_wstage);
    fprintf(f, "river: on=%d dir=%d halfw=%.2f c=%.2f amp=%.2f freq=%.4f ph=%.3f "
               "water_y=%.2f depth=%.2f bank=%.1f\n",
            g_river_on, g_river_dir, g_river_w, g_river_c, g_river_amp, g_river_freq, g_river_ph,
            WATER_Y, RIVER_D, RIVER_BANK);
    fprintf(f, "me: across=%.2f ground=%.2f eye_y=%.2f\n",
            RiverAcross(g_eye.x, g_eye.z), GroundH(g_eye.x, g_eye.z), g_eye.y);
    {   /* 世界顶点的包围盒：验证重攒之后东西是不是还在人周围 */
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        UINT i2;

        for (i2 = 0; i2 < g_wn; i2++) {
            int kk;

            for (kk = 0; kk < 3; kk++) {
                float v = (&g_wbuf[i2].pos.x)[kk];

                if (v < mn[kk]) mn[kk] = v;
                if (v > mx[kk]) mx[kk] = v;
            }
        }
        fprintf(f, "world bbox: x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n",
                mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }
    {   /* 她的世界包围盒：用来验证有没有穿地、头有多高 */
        UINT i2;
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };

        for (i2 = 0; i2 < g_vn; i2++) {
            int kk;

            for (kk = 0; kk < 3; kk++) {
                float v = (&g_vbuf_a[i2].pos.x)[kk];

                if (v < mn[kk]) mn[kk] = v;
                if (v > mx[kk]) mx[kk] = v;
            }
        }
        fprintf(f, "woman bbox at build: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f  (verts=%u)\n",
                mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], g_vn);
        fprintf(f, "  pose=%s\n", (g_pose == POSE_SIT) ? "sit" : "stand");
    }    fprintf(f, "woman_at=(%.3f, %.3f, %.3f) yaw=%.3f live=%d\n",
            g_woman_at.x, g_woman_at.y, g_woman_at.z, g_woman_yaw, g_woman_live);
    fprintf(f, "eye=(%.3f, %.3f, %.3f) yaw=%.4f pitch=%.4f\n",
            g_eye.x, g_eye.y, g_eye.z, g_yaw, g_pitch);
    fprintf(f, "dbg: nargs=%d shot=%d log=%d bend=%d dz=%.2f wz=%.2f wyaw=%.3f set_yaw=%d "
               "set_pitch=%d look=%d\n",
            g_nargs, g_dbg_shot_on, g_dbg_log_on, g_dbg_bend, g_dbg_dist, g_dbg_wz,
            g_dbg_wyaw, g_dbg_set_yaw, g_dbg_set_pitch, g_dbg_set_look);
    {
        float dx = g_woman_at.x - g_eye.x, dz = g_woman_at.z - g_eye.z;
        float dist = sqrtf(dx * dx + dz * dz);
        float ang = atan2f(dx, dz);                       /* 她在哪个方向 */

        fprintf(f, "dist=%.3f bearing=%.4f bearing_rel=%.4f (0=正前方)\n",
                dist, ang, ang - g_yaw);
    }
    {   /* 附近几棵树：验证碰撞箱和看见的树是不是同一个位置 */
        int tx = (int)floorf(g_eye.x / TILE), tz = (int)floorf(g_eye.z / TILE);
        int i, j;

        fprintf(f, "trees near (%d,%d):\n", tx, tz);
        for (i = -1; i <= 1; i++)
            for (j = -1; j <= 1; j++) {
                XMFLOAT3 at;
                float sc;

                if (TileTree(tx + i, tz + j, &at, &sc))
                    fprintf(f, "  tile(%d,%d) at=(%.2f, %.2f) scale=%.2f d=%.3f\n",
                            tx + i, tz + j, at.x, at.z, sc,
                            sqrtf((at.x - g_eye.x) * (at.x - g_eye.x) +
                                  (at.z - g_eye.z) * (at.z - g_eye.z)));
            }
    }
    fclose(f);
}

/* 天空：一个盖住整个屏幕的大三角形，每个顶点带一条"看出去的方向"，
 * 天顶到地平线的渐变、太阳本体和光晕都在像素着色器里算（见 g_sky_hlsl）。 */
static void BuildSkyRays(XMMATRIX view, XMMATRIX proj)
{
    static const float ndc[3][2] = { { -1.0f, -1.0f }, { 3.0f, -1.0f }, { -1.0f, 3.0f } };
    XMMATRIX inv = XMMatrixInverse(NULL, XMMatrixMultiply(view, proj));
    float *dst = NULL;
    D3D11_MAPPED_SUBRESOURCE ms;
    int i;

    for (i = 0; i < 3; i++) {
        XMVECTOR n = XMVectorSet(ndc[i][0], ndc[i][1], 1.0f, 1.0f);
        XMVECTOR w = XMVector4Transform(n, inv);
        XMFLOAT3 wp;

        w = XMVectorScale(w, 1.0f / XMVectorGetW(w));
        XMStoreFloat3(&wp, w);
        g_sky_ray[i] = wp;                    /* 世界坐标下这个角的方向点（相机在原点附近） */
    }

    if (FAILED(g_ctx->Map(g_sky_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return;
    dst = (float *)ms.pData;
    for (i = 0; i < 3; i++) {
        XMFLOAT3 r;

        r.x = g_sky_ray[i].x - g_sky_eye.x;
        r.y = g_sky_ray[i].y - g_sky_eye.y;
        r.z = g_sky_ray[i].z - g_sky_eye.z;
        dst[i * 6 + 0] = ndc[i][0];
        dst[i * 6 + 1] = ndc[i][1];
        dst[i * 6 + 2] = 0.0f;
        dst[i * 6 + 3] = r.x;
        dst[i * 6 + 4] = r.y;
        dst[i * 6 + 5] = r.z;
    }
    g_ctx->Unmap(g_sky_vb, 0);
}

static void DrawSky(void)
{
    UINT stride = sizeof(float) * 6, off = 0;

    g_ctx->IASetInputLayout(g_sky_layout);
    g_ctx->IASetVertexBuffers(0, 1, &g_sky_vb, &stride, &off);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_sky_vs, NULL, 0);
    g_ctx->PSSetShader(g_sky_ps, NULL, 0);
    g_ctx->OMSetDepthStencilState(g_ds_off, 0);          /* 不写深度，后面照常盖上去 */
    g_ctx->Draw(3, 0);
    g_ctx->OMSetDepthStencilState(g_ds_on, 0);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetShader(g_ps, NULL, 0);
}

static void Render(HWND hwnd)
{
    float clear[4] = { 0.53f, 0.72f, 0.92f, 1.0f };     /* 天蓝色 */
    UINT stride = sizeof(Vtx), off = 0;
    D3D11_VIEWPORT vp = { 0, 0, (float)WIN_W, (float)WIN_H, 0.0f, 1.0f };
    XMMATRIX view_now;
    int self_view = (g_dbg_self > 0.0f);

    UpdateInput(hwnd, g_dt);
    WorldStep();                     /* 世界跟着人走：该重攒就攒一点，攒完换缓冲 */

    /* --self：调试用，从外面看自己这副身子（正常玩的时候不会走到这里） */
    if (self_view) {
        XMVECTOR ce = XMVectorSet(g_eye.x + g_dbg_self * 0.60f, g_jy + 1.30f,
                                  g_eye.z + g_dbg_self * 0.80f, 1.0f);
        XMVECTOR ct = XMVectorSet(g_eye.x, g_jy + 0.92f, g_eye.z, 1.0f);

        view_now = XMMatrixLookAtLH(ce, ct, XMVectorSet(0, 1, 0, 0));
    } else {
        view_now = g_view;
    }

    if (!self_view) {
        BuildHud();
        {
            D3D11_MAPPED_SUBRESOURCE ms;

            if (SUCCEEDED(g_ctx->Map(g_hand_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                memcpy(ms.pData, g_vbuf_a, sizeof(Vtx) * g_hand_n);
                g_ctx->Unmap(g_hand_vb, 0);
            }
        }
    }
    BuildSelf();
    {
        D3D11_MAPPED_SUBRESOURCE ms;

        if (SUCCEEDED(g_ctx->Map(g_body_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, g_vbuf_a, sizeof(Vtx) * g_body_n);
            g_ctx->Unmap(g_body_vb, 0);
        }
    }
    if (g_woman_live && (g_bend_amt > 0.0005f || !g_woman_uploaded)) {   /* 只有她要动的时候才重传顶点 */
        BuildWoman(g_woman_at, g_woman_yaw);
        if (g_pose == POSE_LIE)
            g_sit_lift = LieLiftFix();
        else if (g_pose == POSE_SIT)
            g_sit_lift = SitGroundFix();
        if (g_vn != g_woman_n) {         /* 顶点数变了（头发根数会随姿势变），重建缓冲 */
            if (g_woman_vb) {
                g_woman_vb->Release();
                g_woman_vb = NULL;
            }
            g_woman_n = g_vn;
            {
                D3D11_BUFFER_DESC bd;

                ZeroMemory(&bd, sizeof(bd));
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.ByteWidth = sizeof(Vtx) * g_woman_n;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(g_dev->CreateBuffer(&bd, NULL, &g_woman_vb)))
                    return;
            }
        }
        {
            D3D11_MAPPED_SUBRESOURCE ms;

            if (SUCCEEDED(g_ctx->Map(g_woman_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                memcpy(ms.pData, g_vbuf_a, sizeof(Vtx) * g_woman_n);
                g_ctx->Unmap(g_woman_vb, 0);
                g_woman_uploaded = 1;
            }
        }
    }

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_rs);

    /* 先把天和太阳铺上（不吃深度），地面和东西再往上画 */
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_sky_eye = self_view ? XMFLOAT3{ g_eye.x + g_dbg_self * 0.60f, g_jy + 1.30f,
                                      g_eye.z + g_dbg_self * 0.80f } : g_eye;
    BuildSkyRays(view_now, g_proj);
    g_cbd.anim = XMFLOAT4((float)g_clock, FOG_START, FOG_END, 1.0f);
    g_cbd.mvp = XMMatrixMultiply(view_now, g_proj);
    g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
    DrawSky();

    g_ctx->IASetInputLayout(g_layout);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetShader(g_ps, NULL, 0);

    /* 世界：地面 + 草 + 花 + 树 + 太阳。呼吸和头发全在着色器里算，
     * 这里每帧只更新几个常数，顶点一个字节都不用重传。 */
    g_cbd.anim = XMFLOAT4((float)g_clock, FOG_START, FOG_END, 1.0f);
    g_cbd.eye = XMFLOAT4(self_view ? (g_eye.x + g_dbg_self * 0.60f) : g_eye.x,
                         self_view ? (g_jy + 1.30f) : g_eye.y,
                         self_view ? (g_eye.z + g_dbg_self * 0.80f) : g_eye.z, 1.0f);
    g_cbd.mvp = XMMatrixMultiply(view_now, g_proj);
    g_cbd.light.w = 0.42f;                    /* 地面/草的背光面 */
    g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
    if (!g_dbg_handsonly && g_world_n[g_world_cur] > 0) {
        /* 只画到水面之前：水面在缓冲区最后，要留到后面单独用半透明画，
         * 不然它先被不透明地画一遍、还写了深度，后面那一遍就盖不上去了 */
        UINT solid = (UINT)g_water_off;

        if (solid == 0 || solid > g_world_n[g_world_cur])
            solid = g_world_n[g_world_cur];
        g_ctx->IASetVertexBuffers(0, 1, &g_world_vb[g_world_cur], &stride, &off);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->Draw(solid, 0);
    }

    /* 她自己一个绘制：和世界一样吃深度。
     * 环境光调高一点：不然她背光的那半身会黑成一团，什么都看不清。 */
    if (g_woman_live && g_woman_n > 0) {
        g_cbd.light.w = 0.80f;
        g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
        g_ctx->IASetVertexBuffers(0, 1, &g_woman_vb, &stride, &off);
        g_ctx->Draw(g_woman_n, 0);
    }

    /* 腿：和世界一样吃深度，所以低头能看见、也会被草挡住一点 */
    g_cbd.mvp = XMMatrixMultiply(view_now, g_proj);
    g_cbd.light.w = 0.55f;
    g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
    g_ctx->IASetVertexBuffers(0, 1, &g_body_vb, &stride, &off);
    g_ctx->Draw(g_body_n, 0);

    /* 水面单独画一遍（放在身子后面）：半透明，能看见水底的石头和河床，
     * 站在水里的时候腿也会被水盖住一层颜色 —— 所以河看着是有深度的。
     * 它的顶点排在缓冲区最后（g_water_off/g_water_n）：吃深度测试、但不写深度。 */
    if (g_water_n > 0) {
        UINT woff = (UINT)g_water_off * stride;

        g_ctx->OMSetBlendState(g_blend, NULL, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(g_ds_nowrite, 0);
        g_cbd.anim.w = 0.70f;                     /* 顶点里没给透明度的时候用这个 */
        g_cbd.light.w = 0.85f;
        g_cbd.mvp = XMMatrixMultiply(view_now, g_proj);
        g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
        g_ctx->IASetVertexBuffers(0, 1, &g_world_vb[g_world_cur], &stride, &woff);
        g_ctx->Draw(g_water_n, 0);
        g_ctx->OMSetDepthStencilState(g_ds_on, 0);
        g_ctx->OMSetBlendState(NULL, NULL, 0xFFFFFFFF);
        g_cbd.anim.w = 1.0f;
    }

    /* 双手：已经摆在镜头前（世界空间），和世界共用矩阵，但不吃深度 */
    if (!self_view) {
        g_ctx->OMSetDepthStencilState(g_ds_off, 0);
        g_cbd.mvp = XMMatrixMultiply(view_now, g_proj);
        g_cbd.light.w = 0.55f;
        g_ctx->UpdateSubresource(g_cb, 0, NULL, &g_cbd, 0, 0);
        g_ctx->IASetVertexBuffers(0, 1, &g_hand_vb, &stride, &off);
        g_ctx->Draw(g_hand_n, 0);
        g_ctx->OMSetDepthStencilState(g_ds_on, 0);
    }

    /* 截图必须赶在 Present 之前：DXGI_SWAP_EFFECT_DISCARD 的后备缓冲
     * Present 之后内容就作废了，再去拷会拷到上一帧甚至垃圾。 */
    if (g_dbg_shot_on)
        DbgDump(g_dbg_shot);

    g_swap->Present(0, 0);        /* 不锁帧：垂直同步关掉 */
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (m == WM_RBUTTONDOWN) {                 /* 右键：指着她的话让她弯腰 */
        g_rmb = 1.0f;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {   /* 只有 ESC：退出 */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    RECT rc = { 0, 0, WIN_W, WIN_H };

    (void)prev; (void)cmd;
    ParseDbgArgs();
    if (g_dbg_pose >= 0)          /* 姿势要在建模型之前定下来 */
        g_pose = g_dbg_pose;
    g_eye.x = g_dbg_px;           /* --px/--pz：一开始就站在世界坐标的某处 */
    g_eye.z = g_dbg_pz;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"fpclass";
    RegisterClassExW(&wc);

    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, L"fpclass", L"第一人称",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, show);
    ShowCursor(FALSE);

    if (!InitD3D(hwnd)) {
        if (!g_errmsg[0])
            wsprintfW(g_errmsg, L"Direct3D 11 初始化失败（没有更详细的信息）");
        MessageBoxW(hwnd, g_errmsg, L"出错", MB_OK);
        return 1;
    }

    /* 命令行指定了角度就照着来（调试/截图用），不然就是 +Z 方向 */
    if (g_dbg_pose >= 0)
        g_pose = g_dbg_pose;
    if (g_dbg_set_yaw)
        g_yaw = g_dbg_yaw;
    if (g_dbg_set_pitch)
        g_pitch = g_dbg_pitch;
    if (g_dbg_set_look) {          /* 镜头直接对准她 */
        float dx = g_woman_at.x - g_eye.x, dz = g_woman_at.z - g_eye.z;

        g_yaw = atan2f(dx, dz);
        g_pitch = atan2f(0.95f - g_eye.y, sqrtf(dx * dx + dz * dz));
    }
    if (g_dbg_bend)
        g_bend_amt = 1.0f;
    if (g_dbg_log_on)
        DbgLog();

    {
        LARGE_INTEGER fq, prev, now;

        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&prev);

    while (1) {
        QueryPerformanceCounter(&now);
        if (g_dbg_bend || g_dbg_shot_on) {
            g_dt = g_fixed_dt;                /* 调试截图：时间固定，结果可复现 */
            g_clock += g_dt;
        } else {
            g_dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)fq.QuadPart);
            if (g_dt > 0.05f)
                g_dt = 0.05f;                     /* 卡了一下也别让人瞬移 */
            if (g_dt <= 0.0001f)
                g_dt = 1.0f / 1000.0f;
        }
        prev = now;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto quit;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        {
            LARGE_INTEGER r0, r1;              /* 只量 Render 本身，日志里报个平均 */

            QueryPerformanceCounter(&r0);
            Render(hwnd);
            QueryPerformanceCounter(&r1);
            g_perf_sum += (double)(r1.QuadPart - r0.QuadPart) / (double)fq.QuadPart;
            g_perf_n++;
        }
        if (g_dbg_shot_on) {
            static int shown = 0;

            if (++shown < g_dbg_frames)
                continue;                          /* --frames：先空跑几帧再截图 */
            if (g_dbg_log_on) {                    /* 顺手把这一帧的状态记下来 */
                FILE *lf = NULL;

                if (fopen_s(&lf, g_dbg_log, "a") == 0 && lf) {
                    fprintf(lf, "render: bend=%.4f woman_n=%u vn=%u upload=%d live=%d wyaw=%.4f\n",
                            g_bend_amt, g_woman_n, g_vn, g_woman_uploaded, g_woman_live, g_woman_yaw);
                    fprintf(lf, "  eye=(%.3f,%.3f,%.3f) woman=(%.3f,%.3f,%.3f) "
                                "dist=%.3f yaw=%.4f pitch=%.4f\n",
                            g_eye.x, g_eye.y, g_eye.z,
                            g_woman_at.x, g_woman_at.y, g_woman_at.z,
                            sqrtf((g_woman_at.x - g_eye.x) * (g_woman_at.x - g_eye.x) +
                                  (g_woman_at.z - g_eye.z) * (g_woman_at.z - g_eye.z)),
                            g_yaw, g_pitch);
                    fprintf(lf, "render avg = %.3f ms over %d frames  (world=%u verts)\n",
                            (g_perf_n > 0) ? (g_perf_sum * 1000.0 / g_perf_n) : 0.0,
                            g_perf_n, g_world_n[g_world_cur]);
                    fclose(lf);
                }
            }
            goto quit;
        }
    }
    }

quit:
    if (g_world_vb[0]) g_world_vb[0]->Release();
    if (g_world_vb[1]) g_world_vb[1]->Release();
    if (g_hand_vb)  g_hand_vb->Release();
    if (g_body_vb)  g_body_vb->Release();
    if (g_woman_vb) g_woman_vb->Release();
    if (g_ds_on)    g_ds_on->Release();
    if (g_ds_off)   g_ds_off->Release();
    if (g_rs)       g_rs->Release();
    if (g_cb)       g_cb->Release();
    if (g_layout)   g_layout->Release();
    if (g_ps)       g_ps->Release();
    if (g_vs)       g_vs->Release();
    if (g_dsv)      g_dsv->Release();
    if (g_rtv)      g_rtv->Release();
    if (g_swap)     g_swap->Release();
    if (g_ctx)      g_ctx->Release();
    if (g_dev)      g_dev->Release();
    return 0;
}
