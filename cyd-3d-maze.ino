#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "monster_sprites_rle.h"
#include "game_over_image.h"

Preferences prefs;

// バイオーム（迷路のテーマ・カラー）定義
struct BiomeTheme {
  uint8_t wallR, wallG, wallB;
  uint8_t darkR, darkG, darkB;
  uint8_t mortR, mortG, mortB;
  uint8_t ceilR, ceilG, ceilB;
  uint8_t floorR, floorG, floorB;
  const char* name;
};

const BiomeTheme biomes[3] = {
  { 185,  50,  40,  145,  35,  28,  220, 205, 185,   15,  20,  40,   35,  35,  45, "RED BRICK MAZE" },   // B1F-B4F
  {  40, 140, 220,   25,  90, 160,  200, 235, 255,   10,  30,  70,   60,  80, 100, "ICE CRYSTAL CAVE" }, // B5F-B9F
  { 140,  40, 180,   90,  20, 130,  230, 170, 250,   30,  10,  45,   45,  25,  55, "ANCIENT RUINS" }     // B10F+
};

// ============================================================================
//  CYD (Cheap Yellow Display ESP32-2432S028R) LovyanGFX ディスプレイ設定
// ============================================================================
class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel_instance; // ST7789 Driver for 2-USB Port CYD Board
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;

public:
  LGFX_CYD() {
    { // SPIバス設定
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000; // 安定動作用27MHz SPIクロック
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    { // ST7789 パネル設定
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = -1;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false; // ST7789の標準カラー設定
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;
      _panel_instance.config(cfg);
    }
    { // バックライトPWM設定
      auto cfg = _light_instance.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq   = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX_CYD lcd;
LGFX_Sprite img(&lcd);
LGFX_Sprite topHud(&lcd);
LGFX_Sprite botHud(&lcd);

#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3700
#define TOUCH_MIN_Y 300
#define TOUCH_MAX_Y 3800

#define LCD_WIDTH       320
#define LCD_HEIGHT      240
#define TOP_HUD_HEIGHT  35
#define BOT_HUD_HEIGHT  35
#define TFT_WIDTH       LCD_WIDTH
#define TFT_HEIGHT      (LCD_HEIGHT - TOP_HUD_HEIGHT - BOT_HUD_HEIGHT) // 170
#define OFFSET_Y        TOP_HUD_HEIGHT                               // 35

SPIClass touchSpi = SPIClass(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

int clockHour = 0;
int clockMin  = 0;
int clockSec  = 0;
unsigned long lastClockTick = 0; 

uint16_t myColor(uint8_t r, uint8_t g, uint8_t b) {
  return lcd.color565(r, g, b);
}

uint16_t fadeColor(uint8_t r, uint8_t g, uint8_t b, float factor) {
  uint8_t fr = (uint8_t)constrain(r * factor, 0.0f, 255.0f);
  uint8_t fg = (uint8_t)constrain(g * factor, 0.0f, 255.0f);
  uint8_t fb = (uint8_t)constrain(b * factor, 0.0f, 255.0f);
  return lcd.color565(fr, fg, fb);
}

// ============================================================================
//  3Dレイキャスティング迷路エンジンおよびオートパイロット制御
// ============================================================================
#define MAP_WIDTH  16
#define MAP_HEIGHT 16

int worldMap[MAP_HEIGHT][MAP_WIDTH] = {0};

extern bool graveActive;
extern int dungeonFloor;
extern int graveDeepestFloor;
void resumeExplore3D();

void generateRandomMaze() {
  for (int y = 0; y < MAP_HEIGHT; y++) {
    for (int x = 0; x < MAP_WIDTH; x++) {
      if (x == 0 || x == MAP_WIDTH - 1 || y == 0 || y == MAP_HEIGHT - 1) {
        worldMap[y][x] = 1;
      } else {
        worldMap[y][x] = (random(0, 100) < 25) ? 1 : 0;
      }
    }
  }

  worldMap[1][1] = 0; worldMap[1][2] = 0; worldMap[2][1] = 0;
  worldMap[14][14] = 2; worldMap[14][13] = 0; worldMap[13][14] = 0; // 2 = Stairs Down

  bool shouldSpawnGrave = graveActive && (dungeonFloor == 1 || dungeonFloor == graveDeepestFloor);
  if (shouldSpawnGrave) {
    worldMap[9][9] = 3;
    worldMap[9][8] = 0;
    worldMap[8][9] = 0;
  }

  // 孤立防止処理: スタート(1,1)から墓(9,9)および階段(14,14)への動線を確保
  if (shouldSpawnGrave) {
    int cx = 1, cy = 1;
    while (cx != 9 || cy != 9) {
      if (cx < 9 && (random(0, 2) == 0 || cy == 9)) cx++;
      else if (cy < 9) cy++;
      if (worldMap[cy][cx] == 1) worldMap[cy][cx] = 0;
    }
  }

  int cx = 1, cy = 1;
  while (cx != 14 || cy != 14) {
    if (cx < 14 && (random(0, 2) == 0 || cy == 14)) cx++;
    else if (cy < 14) cy++;
    if (worldMap[cy][cx] == 1) worldMap[cy][cx] = 0;
  }

  int trapCount = 0;
  while (trapCount < 4) {
    int rx = random(1, MAP_WIDTH - 1);
    int ry = random(1, MAP_HEIGHT - 1);
    if (worldMap[ry][rx] == 0 && (rx != 1 || ry != 1) && (rx != 14 || ry != 14) && (rx != 9 || ry != 9)) {
      worldMap[ry][rx] = 4; // 4 = Poison Trap
      trapCount++;
    }
  }
}

struct Camera {
  float posX, posY;
  float dirX, dirY;
  float planeX, planeY;
};

Camera cam = {
  1.5f, 1.5f,        // posX, posY
  0.0f, 1.0f,        // dirX, dirY（南向き）
  -0.66f, 0.0f       // planeX, planeY
};

SemaphoreHandle_t dataMutex;
TaskHandle_t Task1;

const int dx[4] = {0, 1, 0, -1};
const int dy[4] = {-1, 0, 1, 0};
int currentDir = 2; // Facing South

enum MotionState { IDLE, MOVING_FORWARD, TURNING };
MotionState motionState = MOVING_FORWARD;

int targetGridX = 1, targetGridY = 2;
float animProgress = 0.0f;
int startDir = 2, endDir = 2;

float startPosX = 1.5f, startPosY = 1.5f;
float targetPosX = 1.5f, targetPosY = 2.5f;

void setCameraDirection(float angle) {
  cam.dirX = cosf(angle);
  cam.dirY = sinf(angle);
  cam.planeX = -cam.dirY * 0.66f;
  cam.planeY = cam.dirX * 0.66f;
}

int decideNextDirection(int curX, int curY, int curD) {
  // 優先目標設定: 墓が有効なら墓(3)、回収済みなら階段(2)を目指す
  int searchTile = (graveActive) ? 3 : 2;

  int targetX = -1, targetY = -1;
  for (int y = 0; y < MAP_HEIGHT; y++) {
    for (int x = 0; x < MAP_WIDTH; x++) {
      if (worldMap[y][x] == searchTile) {
        targetX = x;
        targetY = y;
        break;
      }
    }
    if (targetX != -1) break;
  }

  // 墓を探していたが見つからない場合は階段を目指す
  if (targetX == -1 && searchTile == 3) {
    for (int y = 0; y < MAP_HEIGHT; y++) {
      for (int x = 0; x < MAP_WIDTH; x++) {
        if (worldMap[y][x] == 2) {
          targetX = x;
          targetY = y;
          break;
        }
      }
      if (targetX != -1) break;
    }
  }

  if (targetX != -1 && (curX != targetX || curY != targetY)) {
    // 長距離の経路探索時におけるオーバーフロー防止のため int16_t を使用
    int16_t dist[MAP_HEIGHT][MAP_WIDTH];
    int16_t parentDir[MAP_HEIGHT][MAP_WIDTH];
    for (int y = 0; y < MAP_HEIGHT; y++) {
      for (int x = 0; x < MAP_WIDTH; x++) {
        dist[y][x] = -1;
        parentDir[y][x] = -1;
      }
    }

    uint8_t queueX[MAP_HEIGHT * MAP_WIDTH];
    uint8_t queueY[MAP_HEIGHT * MAP_WIDTH];
    int head = 0, tail = 0;

    dist[curY][curX] = 0;
    queueX[tail] = curX;
    queueY[tail] = curY;
    tail++;

    bool found = false;
    while (head < tail) {
      int cx = queueX[head];
      int cy = queueY[head];
      head++;

      if (cx == targetX && cy == targetY) {
        found = true;
        break;
      }

      for (int i = 0; i < 4; i++) {
        int d = (curD + i) % 4;
        int nx = cx + dx[d];
        int ny = cy + dy[d];

        if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
          if (worldMap[ny][nx] != 1 && dist[ny][nx] == -1) {
            dist[ny][nx] = dist[cy][cx] + 1;
            parentDir[ny][nx] = (cx == curX && cy == curY) ? d : parentDir[cy][cx];
            queueX[tail] = nx;
            queueY[tail] = ny;
            tail++;
          }
        }
      }
    }

    if (found && parentDir[targetY][targetX] != -1) {
      return parentDir[targetY][targetX];
    }
  }

  // 目標が確定していない場合は右手法（壁沿い自動移動）
  int rightD = (curD + 1) % 4;
  int frontD = curD;
  int leftD  = (curD + 3) % 4;
  int backD  = (curD + 2) % 4;

  auto isFreeTile = [](int x, int y) {
    return (x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT && worldMap[y][x] != 1);
  };

  if (isFreeTile(curX + dx[rightD], curY + dy[rightD])) return rightD;
  if (isFreeTile(curX + dx[frontD], curY + dy[frontD])) return frontD;
  if (isFreeTile(curX + dx[leftD],  curY + dy[leftD]))  return leftD;
  return backD;
}

static uint8_t vignetteLUT[TFT_WIDTH];
static bool vignetteLUTInited = false;

void initVignetteLUT() {
  float centerX = TFT_WIDTH * 0.5f;
  for (int x = 0; x < TFT_WIDTH; x++) {
    float dx = x - centerX;
    float v = 1.0f - (dx * dx) / (centerX * centerX * 1.35f);
    if (v < 0.25f) v = 0.25f;
    vignetteLUT[x] = (uint8_t)(v * 255.0f);
  }
  vignetteLUTInited = true;
}

// ============================================================================
//  RPGデータ構造（主人公ステータス、装備データベース、ショップ/宝箱画面モード）
// ============================================================================
enum AppMode { MODE_EXPLORE_3D, MODE_BATTLE_FLASH, MODE_BATTLE_SCREEN, MODE_SHOP_SCREEN, MODE_CHEST_SCREEN, MODE_LEVEL_UP_FLASH, MODE_COLLECTION_VIEW, MODE_GAME_OVER, MODE_GRAVE_SCREEN };
AppMode appMode = MODE_EXPLORE_3D;

enum ItemRarity { RARITY_COMMON, RARITY_RARE, RARITY_EPIC, RARITY_LEGENDARY };

struct HackItem {
  char name[48];
  ItemRarity rarity;
  int atkBonus;
  int defBonus;
  char effectDesc[32];
};

HackItem collectionList[6] = {
  {"[COMMON] Rusty Shortsword", RARITY_COMMON, 3, 0, "Basic blade"},
  {"[RARE] Vampiric Broadsword", RARITY_RARE, 12, 0, "Absorbs HP on hit"},
  {"[EPIC] Holy Aegis Shield", RARITY_EPIC, 0, 18, "Holy protection"},
  {"[LEGENDARY] Godly Dragon Blade", RARITY_LEGENDARY, 35, 5, "Ultimate divine power"}
};
int collectionCount = 4;

const char* prefixes[4] = { "Sharpened", "Vampiric", "Demonic", "Godly" };
const char* baseNames[4] = { "Dagger", "Longsword", "Greatshield", "Relic Ring" };

void dropLootItem() {
  if (collectionCount >= 6) {
    for (int i = 0; i < 5; i++) collectionList[i] = collectionList[i + 1];
    collectionCount = 5;
  }
  
  int r = random(0, 100);
  ItemRarity rarity = (r < 12) ? RARITY_LEGENDARY : ((r < 32) ? RARITY_EPIC : ((r < 65) ? RARITY_RARE : RARITY_COMMON));
  const char* prefix = prefixes[random(0, 4)];
  const char* baseName = baseNames[random(0, 4)];
  
  const char* rStr = (rarity == RARITY_LEGENDARY) ? "LEGENDARY" : ((rarity == RARITY_EPIC) ? "EPIC" : ((rarity == RARITY_RARE) ? "RARE" : "COMMON"));
  snprintf(collectionList[collectionCount].name, sizeof(collectionList[collectionCount].name), "[%s] %s %s", rStr, prefix, baseName);
  
  collectionList[collectionCount].rarity = rarity;
  collectionList[collectionCount].atkBonus = (rarity + 1) * 8 + random(1, 6);
  collectionList[collectionCount].defBonus = (rarity + 1) * 4 + random(0, 4);
  const char* eff = (rarity >= RARITY_EPIC) ? "+Vampiric Burst" : "+Basic Power";
  snprintf(collectionList[collectionCount].effectDesc, sizeof(collectionList[collectionCount].effectDesc), "%s", eff);
  
  collectionCount++;
}

// 装備品データベース
struct EquipmentItem {
  const char* name;
  int atkBonus;
  int defBonus;
  int price;
};

const EquipmentItem weaponDatabase[] = {
  {"Rusty Dagger", 2, 0, 0},
  {"Iron Broadsword", 8, 0, 120},
  {"Dragon Blade", 22, 0, 450}
};

const EquipmentItem armorDatabase[] = {
  {"Leather Vest", 0, 2, 0},
  {"Chainmail Armor", 0, 6, 150},
  {"Aegis Shield", 0, 15, 500}
};

// プレイヤー（主人公）の基本ステータス
struct HeroStatus {
  int level;
  int hp, maxHP;
  int mp, maxMP;
  int baseATK, baseDEF;
  int exp, nextEXP;
  int gold;
  int weaponId;
  int armorId;
};

HeroStatus hero = {
  1,        // level
  100, 100, // hp, maxHP
  30, 30,   // mp, maxMP
  18, 6,    // baseATK, baseDEF
  0, 80,    // exp, nextEXP
  50,       // gold
  0, 0      // weaponId, armorId
};

int getHeroTotalATK() {
  int bestLootAtk = 0;
  for (int i = 0; i < collectionCount; i++) {
    if (collectionList[i].atkBonus > bestLootAtk) bestLootAtk = collectionList[i].atkBonus;
  }
  return hero.baseATK + max(weaponDatabase[hero.weaponId].atkBonus, bestLootAtk);
}

int getHeroTotalDEF() {
  int bestLootDef = 0;
  for (int i = 0; i < collectionCount; i++) {
    if (collectionList[i].defBonus > bestLootDef) bestLootDef = collectionList[i].defBonus;
  }
  return hero.baseDEF + max(armorDatabase[hero.armorId].defBonus, bestLootDef);
}

// 戦闘・イベント関連の状態変数
unsigned long battleTimer = 0;
int battleStep = 0;
int monsterType = 0; // 0: Demon, 1: Eldritch, 2: Gargoyle, 3: Hellhound, 4: Gold Slime, 5: Demon Lord Boss, 6: Merchant, 7: Chest
bool isCriticalHit = false;
bool isRareEncounter = false;
bool isBossEncounter = false;
bool isSpellAttack = false;
int spellType = 0;
int monsterHP = 0, monsterMaxHP = 0, monsterATK = 0, monsterEXP = 0, monsterGold = 0;
int lastDamageDealt = 0;
int lastMonsterDamage = 0;
unsigned long monsterHitFlashUntil = 0;
unsigned long heroHitFlashUntil = 0;
int dungeonFloor = 1;
int totalStepsTaken = 0;
bool bossDefeatedOnFloor = false;

// 宝箱・ショップの報酬用メッセージ
const char* chestItemObtained = "";
int chestGoldObtained = 0;
char shopActionMessage[64] = "";
char shopDetailMessage[64] = "";

// 墓システムおよび死亡カウンター
#define BOOT_BTN_PIN 0
bool   graveActive = false;
int    graveDeepestFloor = 0;
int    graveBestFloor = 0;          // 歴代最深記録
int    totalDeaths = 0;             // 累計死亡回数
HackItem graveItems[6];
int    graveItemCount = 0;
int    graveD20Result = 0;
int    graveRecoveredCount = 0;
unsigned long graveTimer = 0;

int    graveWeaponId = 0;
int    graveArmorId  = 0;

void saveGrave() {
  prefs.begin("grave", false);
  prefs.putBool("active", graveActive);
  prefs.putInt("floor",  graveDeepestFloor);
  prefs.putInt("best",   graveBestFloor);
  prefs.putInt("deaths", totalDeaths);
  prefs.putInt("count",  graveItemCount);
  prefs.putInt("wep",    graveWeaponId);
  prefs.putInt("arm",    graveArmorId);
  for (int i = 0; i < graveItemCount; i++) {
    char key[12];
    snprintf(key, sizeof(key), "item%d", i);
    prefs.putBytes(key, &graveItems[i], sizeof(HackItem));
  }
  prefs.end();
}

void loadGrave() {
  prefs.begin("grave", true);
  graveActive        = prefs.getBool("active", false);
  graveDeepestFloor  = prefs.getInt("floor",  0);
  graveBestFloor     = prefs.getInt("best",   0);
  totalDeaths        = prefs.getInt("deaths", 0);
  graveItemCount     = prefs.getInt("count",  0);
  if (graveItemCount > 6) graveItemCount = 6;
  if (graveItemCount < 0) graveItemCount = 0;
  graveWeaponId      = constrain(prefs.getInt("wep", 0), 0, 2);
  graveArmorId       = constrain(prefs.getInt("arm", 0), 0, 2);
  for (int i = 0; i < graveItemCount; i++) {
    char key[12];
    snprintf(key, sizeof(key), "item%d", i);
    prefs.getBytes(key, &graveItems[i], sizeof(HackItem));
  }
  prefs.end();
}

// ----------------------------------------------------------------------------
//  簡易セーブ機能（階層変更時にNVSへ進行状況を保存）
//  ※墓(grave)データとは別の名前空間("save")に保存。
//    ダンジョン構造はランダム生成されるため、階層番号・ステータス・所持品のみ保存。
// ----------------------------------------------------------------------------
void saveGame() {
  prefs.begin("save", false);
  prefs.putBool("valid",    true);
  prefs.putInt("floor",     dungeonFloor);
  prefs.putBool("bossdef",  bossDefeatedOnFloor);
  prefs.putInt("steps",     totalStepsTaken);
  prefs.putInt("level",     hero.level);
  prefs.putInt("hp",        hero.hp);
  prefs.putInt("maxhp",     hero.maxHP);
  prefs.putInt("mp",        hero.mp);
  prefs.putInt("maxmp",     hero.maxMP);
  prefs.putInt("atk",       hero.baseATK);
  prefs.putInt("def",       hero.baseDEF);
  prefs.putInt("exp",       hero.exp);
  prefs.putInt("nextexp",   hero.nextEXP);
  prefs.putInt("gold",      hero.gold);
  prefs.putInt("wep",       hero.weaponId);
  prefs.putInt("arm",       hero.armorId);
  prefs.putInt("itemcount", collectionCount);
  for (int i = 0; i < collectionCount; i++) {
    char key[12];
    snprintf(key, sizeof(key), "item%d", i);
    prefs.putBytes(key, &collectionList[i], sizeof(HackItem));
  }
  prefs.end();
}

// 戻り値: true = 有効なセーブデータを読み込んだ, false = セーブデータなし(初回起動など)
bool loadGame() {
  prefs.begin("save", true);
  bool valid = prefs.getBool("valid", false);
  if (!valid) {
    prefs.end();
    return false;
  }
  dungeonFloor        = prefs.getInt("floor",     1);
  bossDefeatedOnFloor = prefs.getBool("bossdef",  false);
  totalStepsTaken     = prefs.getInt("steps",     0);
  hero.level       = prefs.getInt("level",     1);
  hero.hp          = prefs.getInt("hp",        50);
  hero.maxHP       = prefs.getInt("maxhp",     50);
  hero.mp          = prefs.getInt("mp",        10);
  hero.maxMP       = prefs.getInt("maxmp",     10);
  hero.baseATK     = prefs.getInt("atk",       12);
  hero.baseDEF     = prefs.getInt("def",       8);
  hero.exp         = prefs.getInt("exp",       0);
  hero.nextEXP     = prefs.getInt("nextexp",   100);
  hero.gold        = prefs.getInt("gold",      50);
  hero.weaponId    = constrain(prefs.getInt("wep", 0), 0, 2);
  hero.armorId     = constrain(prefs.getInt("arm", 0), 0, 2);
  collectionCount  = prefs.getInt("itemcount", 0);
  if (collectionCount > 6) collectionCount = 6;
  if (collectionCount < 0) collectionCount = 0;
  for (int i = 0; i < collectionCount; i++) {
    char key[12];
    snprintf(key, sizeof(key), "item%d", i);
    prefs.getBytes(key, &collectionList[i], sizeof(HackItem));
  }
  prefs.end();
  return true;
}

// 新しい生涯が始まる際(死亡後リセット時)は、古い進行中セーブを無効化する。
// ※墓(grave)データはここではクリアしない。あくまで「階層進行状況」のみ無効化。
void clearSaveGame() {
  prefs.begin("save", false);
  prefs.putBool("valid", false);
  prefs.end();
}

// 死亡処理（BOOTボタン押下 / 通常戦闘敗北時の共通処理）
void triggerHeroDeath() {
  hero.hp = 0;
  totalDeaths++;
  graveActive = true;
  graveDeepestFloor = dungeonFloor;
  if (dungeonFloor > graveBestFloor) graveBestFloor = dungeonFloor;
  isBossEncounter = false;
  isRareEncounter = false;
  // 前世代の墓データをクリアし、現世代のデータで更新
  memset(graveItems, 0, sizeof(graveItems));
  graveItemCount = collectionCount;
  graveWeaponId = hero.weaponId;
  graveArmorId = hero.armorId;
  for (int i = 0; i < graveItemCount; i++) graveItems[i] = collectionList[i];
  saveGrave();
  clearSaveGame(); // この生涯の階層進行セーブは死亡により無効化(墓データとは独立)
  appMode = MODE_GAME_OVER;
  battleTimer = millis();
}

// トラップ・回復・バイオーム・リミットゲージ・コンボ状態
unsigned long trapFlashUntil = 0;
int trapDamageTaken = 0;
bool isHealSpell = false;
int healAmountDealt = 0;
int limitGauge = 0;
bool isLimitOverdrive = false;
unsigned long slashEffectUntil = 0;
int comboHits = 1;

inline const BiomeTheme& getCurrentBiome() {
  if (dungeonFloor < 5) return biomes[0];
  if (dungeonFloor < 10) return biomes[1];
  return biomes[2];
}

// ターン制戦闘ループの状態定義
enum BattlePhase { BP_INTRO, BP_HERO_ATTACK, BP_MONSTER_COUNTER, BP_VICTORY, BP_DEFEAT };
BattlePhase battlePhase = BP_INTRO;
unsigned long phaseTimer = 0;
int battleTurn = 1;
bool heroWasDefeated = false;

// ----------------------------------------------------------------------------
//  モンスター難易度・ステータスバランス設定
// ----------------------------------------------------------------------------
struct MonsterTier { float turnsToKill; float dmgFrac; int expBase, goldBase; };

const MonsterTier monsterTiers[12] = {
  { 2.2f, 0.065f,  45,  35 }, // 0: Shadow Demon
  { 3.0f, 0.085f,  75,  55 }, // 1: Eldritch Beast
  { 3.8f, 0.100f, 120,  85 }, // 2: Void Gargoyle
  { 4.5f, 0.130f, 180, 130 }, // 3: Hellhound
  { 1.8f, 0.025f, 600, 500 }, // 4: Gold Slime
  { 5.0f, 0.130f, 750, 400 }, // 5: Demon Lord Boss
  { 2.0f, 0.050f,   0,   0 }, // 6: Merchant
  { 2.0f, 0.050f,   0,   0 }, // 7: Chest
  { 3.5f, 0.095f, 140, 110 }, // 8: Phoenix Demon
  { 4.2f, 0.120f, 220, 160 }, // 9: Skeleton Bone Dragon
  { 3.6f, 0.110f, 170, 125 }, // 10: Undead Purple Lich
  { 2.6f, 0.085f,  95,  75 }, // 11: Dark Witch
};

void computeMonsterStats(int tier) {
  int hAtk   = getHeroTotalATK();
  int hDef   = getHeroTotalDEF();
  int hMaxHP = hero.maxHP;
  const MonsterTier &t = monsterTiers[tier];

  float floorMul = 1.0f + (dungeonFloor - 1) * 0.05f;

  monsterMaxHP = (int)max((long)15, (long)(hAtk * 1.25f * t.turnsToKill * floorMul));
  monsterATK   = hDef + (int)max((long)3, (long)(hMaxHP * t.dmgFrac * floorMul));
  monsterHP    = monsterMaxHP;
  monsterEXP   = (int)(t.expBase * floorMul);
  monsterGold  = (int)(t.goldBase * floorMul);
}

void resolveHeroAttack() {
  comboHits = 1;
  if (limitGauge >= 100) {
    isLimitOverdrive = true;
    isHealSpell = false;
    isSpellAttack = false;
    isCriticalHit = true;
    comboHits = 2; // LIMIT OVERDRIVE: DOUBLE ULTIMATE!
    lastDamageDealt = (int)(getHeroTotalATK() * 3.8f + random(5, 13));
    int absorbHP = (int)(lastDamageDealt * 0.30f);
    hero.hp = min(hero.maxHP, hero.hp + absorbHP);
    monsterHP -= lastDamageDealt;
    if (monsterHP < 0) monsterHP = 0;
    monsterHitFlashUntil = millis() + 250;
    slashEffectUntil = millis() + 450;
    limitGauge = 0;
  } else if (hero.hp < hero.maxHP / 2 && hero.mp >= 8 && random(0, 100) < 45) {
    isLimitOverdrive = false;
    isHealSpell = true;
    isSpellAttack = false;
    isCriticalHit = false;
    hero.mp -= 8;
    healAmountDealt = (int)(hero.maxHP * 0.45f);
    hero.hp = min(hero.maxHP, hero.hp + healAmountDealt);
    lastDamageDealt = 0;
    slashEffectUntil = 0;
    limitGauge = min(100, limitGauge + 15);
  } else {
    isLimitOverdrive = false;
    isHealSpell = false;
    isSpellAttack = (random(0, 100) < 35 && hero.mp >= 5);
    if (isSpellAttack) {
      hero.mp -= 5;
      spellType = random(0, 2);
      lastDamageDealt = (int)(getHeroTotalATK() * 2.2f);
      isCriticalHit = false;
      comboHits = 1;
    } else {
      // 確率による連撃コンボ判定（4連撃: 4%, 3連撃: 10%, 2連撃: 22%）
      int cRoll = random(0, 100);
      if (cRoll < 4)       comboHits = 4;
      else if (cRoll < 14) comboHits = 3;
      else if (cRoll < 36) comboHits = 2;
      else                 comboHits = 1;

      int singleHit = (int)max((long)1, (long)(getHeroTotalATK() + random(-2, 4)));
      isCriticalHit = (random(0, 100) < 20);
      if (isCriticalHit) singleHit = (int)(singleHit * 1.7f);

      lastDamageDealt = singleHit * comboHits;
    }
    monsterHP -= lastDamageDealt;
    if (monsterHP < 0) monsterHP = 0;
    monsterHitFlashUntil = millis() + 220;
    slashEffectUntil = millis() + 160 + (comboHits * 80);
    limitGauge = min(100, limitGauge + (comboHits * 10));
  }
}

void resolveMonsterCounter() {
  lastMonsterDamage = (int)max((long)1, (long)((monsterATK - getHeroTotalDEF()) + random(-1, 3)));
  hero.hp -= lastMonsterDamage;
  if (hero.hp < 0) hero.hp = 0;
  heroHitFlashUntil = millis() + 220;
  limitGauge = min(100, limitGauge + 12);
}

void drawRLEMonsterSprite(LGFX_Sprite* canvas, int centerX, int centerY, int type, int scale = 2) {
  const RLEEntry* spriteRLE;
  size_t rleLength;

  if (type == 0)      { spriteRLE = scaryDarkDemonSpriteRLE; rleLength = scaryDarkDemonSpriteRLELen; }
  else if (type == 1) { spriteRLE = scaryEldritchBeastSpriteRLE; rleLength = scaryEldritchBeastSpriteRLELen; }
  else if (type == 2) { spriteRLE = darkGargoyleBossSpriteRLE; rleLength = darkGargoyleBossSpriteRLELen; }
  else if (type == 3) { spriteRLE = scaryHellhoundSpriteRLE; rleLength = scaryHellhoundSpriteRLELen; }
  else if (type == 5) { spriteRLE = epicDemonLordBossSpriteRLE; rleLength = epicDemonLordBossSpriteRLELen; }
  else if (type == 6) { spriteRLE = retroMerchantNPCSpriteRLE; rleLength = retroMerchantNPCSpriteRLELen; }
  else if (type == 7) { spriteRLE = retroTreasureChestSpriteRLE; rleLength = retroTreasureChestSpriteRLELen; }
  else if (type == 8) { spriteRLE = phoenixSpriteRLE; rleLength = phoenixSpriteRLELen; }
  else if (type == 9) { spriteRLE = boneDragonSpriteRLE; rleLength = boneDragonSpriteRLELen; }
  else if (type == 10) { spriteRLE = lichSpriteRLE; rleLength = lichSpriteRLELen; }
  else if (type == 11) { spriteRLE = assassinSpriteRLE; rleLength = assassinSpriteRLELen; }
  else                { spriteRLE = scaryDarkDemonSpriteRLE; rleLength = scaryDarkDemonSpriteRLELen; }

  int renderSize = MONSTER_SPRITE_SIZE * scale;
  int startX = centerX - renderSize / 2;
  int startY = centerY - renderSize / 2;

  int pixelIndex = 0;
  for (size_t i = 0; i < rleLength; i++) {
    uint16_t c565 = pgm_read_word(&spriteRLE[i].color);
    uint8_t count = pgm_read_byte(&spriteRLE[i].count);
    if (c565 == 0x0000 || c565 == 0xFFFF) {
      pixelIndex += count;
      continue;
    }

    uint8_t r = ((c565 >> 11) & 0x1F) << 3;
    uint8_t g = ((c565 >> 5) & 0x3F) << 2;
    uint8_t b = (c565 & 0x1F) << 3;

    if (isRareEncounter && type != 6 && type != 7) {
      r = 255; g = 215; b = 30; // Gold Slime Shiny
    } else if (type < 6 || type >= 8) {
      if (dungeonFloor >= 10) {
        // 深層（B10F以上）: 赤/橙系の発光
        r = min(255, (int)(r * 1.3f + 40));
        g = (uint8_t)(g * 0.4f);
        b = (uint8_t)(b * 0.2f);
      } else if (dungeonFloor >= 6) {
        // 中層（B6F-B9F）: 紫/シアン系の発光
        r = (uint8_t)(r * 0.6f);
        g = (uint8_t)(g * 0.5f);
        b = min(255, (int)(b * 1.4f + 45));
      } else if (dungeonFloor <= 2) {
        // 浅層（B1F-B2F）: 落ち着いた影の色合い
        r = (uint8_t)(r * 0.75f);
        g = (uint8_t)(g * 0.75f);
        b = (uint8_t)(b * 0.75f);
      }
    }
    uint16_t finalColor = myColor(r, g, b);

    for (int k = 0; k < count; k++) {
      int px = (pixelIndex + k) % MONSTER_SPRITE_SIZE;
      int py = (pixelIndex + k) / MONSTER_SPRITE_SIZE;

      // 外枠・透過処理（最外周および暗いノイズを除去）
      if (px == 0 || px == MONSTER_SPRITE_SIZE - 1 || py == 0 || py == MONSTER_SPRITE_SIZE - 1) continue;
      if ((px <= 2 || px >= MONSTER_SPRITE_SIZE - 3 || py <= 2 || py >= MONSTER_SPRITE_SIZE - 3) && (r < 65 && g < 65 && b < 65)) continue;

      if (scale == 1) {
        canvas->drawPixel(startX + px, startY + py, finalColor);
      } else {
        canvas->fillRect(startX + px * scale, startY + py * scale, scale, scale, finalColor);
      }
    }
    pixelIndex += count;
  }
}

// ショップ画面の描画処理
void renderShopScreen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  
  img.fillSprite(myColor(5, 20, 35));
  img.drawRect(2, 2, TFT_WIDTH - 4, TFT_HEIGHT - 4, myColor(0, 200, 255));
  img.drawRect(4, 4, TFT_WIDTH - 8, TFT_HEIGHT - 8, myColor(0, 200, 255));

  img.fillRect(8, 6, TFT_WIDTH - 16, 32, TFT_BLACK);
  img.drawRect(8, 6, TFT_WIDTH - 16, 32, myColor(0, 200, 255));

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(TFT_CYAN, TFT_BLACK);
  img.setTextSize(1);
  img.drawString("--- MYSTERIOUS MERCHANT SHOP ---", TFT_WIDTH / 2, 22, 1);

  drawRLEMonsterSprite(&img, 55, TFT_HEIGHT / 2 + 4, 6, 2);

  img.fillRect(115, 46, TFT_WIDTH - 123, 118, TFT_BLACK);
  img.drawRect(115, 46, TFT_WIDTH - 123, 118, myColor(0, 200, 255));

  img.setTextDatum(textdatum_t::middle_left);
  img.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[64];
  
  snprintf(buf, sizeof(buf), "GOLD: %d G", hero.gold);
  img.drawString(buf, 123, 58, 1);

  snprintf(buf, sizeof(buf), "HP:%d/%d  MP:%d/%d", hero.hp, hero.maxHP, hero.mp, hero.maxMP);
  img.drawString(buf, 123, 74, 1);

  snprintf(buf, sizeof(buf), "ATK:%d  DEF:%d", getHeroTotalATK(), getHeroTotalDEF());
  img.drawString(buf, 123, 90, 1);

  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  img.drawString(shopActionMessage, 123, 112, 1);

  img.setTextColor(TFT_GREEN, TFT_BLACK);
  img.drawString(shopDetailMessage, 123, 132, 1);

  xSemaphoreGive(dataMutex);
}

// ----------------------------------------------------------------------------
//  3D 20面ダイス描画処理
// ----------------------------------------------------------------------------
int d20RollResult = 20;

struct Point3D { float x, y, z; };
struct Point2D { int x, y; };

const Point3D d20Vertices[12] = {
  {-0.5257f, 0.0f, 0.8507f}, {0.5257f, 0.0f, 0.8507f}, {-0.5257f, 0.0f, -0.8507f}, {0.5257f, 0.0f, -0.8507f},
  {0.0f, 0.8507f, 0.5257f}, {0.0f, 0.8507f, -0.5257f}, {0.0f, -0.8507f, 0.5257f}, {0.0f, -0.8507f, -0.5257f},
  {0.8507f, 0.5257f, 0.0f}, {-0.8507f, 0.5257f, 0.0f}, {0.8507f, -0.5257f, 0.0f}, {-0.8507f, -0.5257f, 0.0f}
};

const uint8_t d20Faces[20][3] = {
  {0,1,4}, {0,4,9}, {0,9,11}, {0,11,6}, {0,6,1},
  {1,6,10}, {1,10,8}, {1,8,4}, {4,8,5}, {4,5,9},
  {9,5,2}, {9,2,11}, {11,2,7}, {11,7,6}, {6,7,10},
  {8,10,3}, {5,8,3}, {2,5,3}, {7,2,3}, {10,7,3}
};

void draw3DD20Dice(int cx, int cy, float rotA, int rollValue, bool isRolling) {
  Point2D proj[12];
  float scale = 34.0f;
  float cosA = cosf(rotA), sinA = sinf(rotA);

  for (int i = 0; i < 12; i++) {
    float rx = d20Vertices[i].x * cosA - d20Vertices[i].z * sinA;
    float rz = d20Vertices[i].x * sinA + d20Vertices[i].z * cosA;
    float ry = d20Vertices[i].y * cosA - rz * sinA;
    rz = d20Vertices[i].y * sinA + rz * cosA;

    float f = 1.0f / (1.0f + rz * 0.25f);
    proj[i].x = cx + (int)(rx * scale * f);
    proj[i].y = cy + (int)(ry * scale * f);
  }

  uint8_t baseR = isRolling ? 0 : ((rollValue == 20) ? 255 : ((rollValue == 1) ? 220 : 0));
  uint8_t baseG = isRolling ? 180 : ((rollValue == 20) ? 200 : ((rollValue == 1) ? 30 : 200));
  uint8_t baseB = isRolling ? 240 : ((rollValue == 20) ? 20 : ((rollValue == 1) ? 30 : 100));

  uint16_t edgeCol = isRolling ? myColor(0, 220, 255) : ((rollValue == 20) ? myColor(255, 220, 0) : ((rollValue == 1) ? myColor(255, 60, 60) : myColor(0, 240, 140)));

  // 面塗りサーフェス描画（フラットシェーディングおよび裏面カリング）
  for (int f = 0; f < 20; f++) {
    int v0 = d20Faces[f][0];
    int v1 = d20Faces[f][1];
    int v2 = d20Faces[f][2];

    int cross = (proj[v1].x - proj[v0].x) * (proj[v2].y - proj[v0].y) - (proj[v1].y - proj[v0].y) * (proj[v2].x - proj[v0].x);
    if (cross > 0) {
      float shadeFactor = 0.35f + (float)(f % 5) * 0.12f;
      uint16_t faceCol = fadeColor(baseR, baseG, baseB, shadeFactor);
      img.fillTriangle(proj[v0].x, proj[v0].y, proj[v1].x, proj[v1].y, proj[v2].x, proj[v2].y, faceCol);
      img.drawTriangle(proj[v0].x, proj[v0].y, proj[v1].x, proj[v1].y, proj[v2].x, proj[v2].y, edgeCol);
    }
  }

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(TFT_WHITE, TFT_BLACK);
  img.setTextSize(2);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", isRolling ? random(1, 21) : rollValue);
  img.drawString(buf, cx, cy, 1);
  img.setTextSize(1);
}

// 宝箱画面の描画（3D 20面ダイス演出含む）
void renderChestScreen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  
  unsigned long cElapsed = millis() - battleTimer;
  bool isRolling = (cElapsed < 900);
  float rotA = cElapsed * 0.009f;

  img.fillSprite(myColor(25, 15, 5));
  img.fillRect(8, 6, TFT_WIDTH - 16, 32, TFT_BLACK);

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextSize(1);

  if (isRolling) {
    img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
    img.drawString("--- ROLLING 3D D20 DESTINY DICE... ---", TFT_WIDTH / 2, 22, 1);
  } else {
    if (d20RollResult == 20) {
      img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
      img.drawString("★ D20 ROLL: 20 NAT 20 CRITICAL SUCCESS! ★", TFT_WIDTH / 2, 22, 1);
    } else if (d20RollResult == 1) {
      img.setTextColor(myColor(255, 60, 60), TFT_BLACK);
      img.drawString("💀 D20 ROLL: 1 FUMBLE TRAP EXPLODED! 💀", TFT_WIDTH / 2, 22, 1);
    } else {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      char rBuf[64];
      snprintf(rBuf, sizeof(rBuf), "=== D20 ROLL RESULT: %d SUCCESS! ===", d20RollResult);
      img.drawString(rBuf, TFT_WIDTH / 2, 22, 1);
    }
  }

  drawRLEMonsterSprite(&img, TFT_WIDTH / 4 + 10, TFT_HEIGHT / 2 + 4, 7, 2);
  draw3DD20Dice(TFT_WIDTH * 3 / 4 - 10, TFT_HEIGHT / 2 + 4, rotA, d20RollResult, isRolling);

  img.fillRect(8, TFT_HEIGHT - 28, TFT_WIDTH - 16, 22, TFT_BLACK);

  img.setTextDatum(textdatum_t::middle_left);
  char buf[64];
  if (isRolling) {
    img.setTextColor(TFT_YELLOW, TFT_BLACK);
    snprintf(buf, sizeof(buf), "ROLLING D20 FOR LOOT & GOLD...");
  } else {
    if (d20RollResult == 20) {
      img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
      snprintf(buf, sizeof(buf), "JACKPOT! +%dG & LEGENDARY LOOT!", chestGoldObtained);
    } else if (d20RollResult == 1) {
      img.setTextColor(myColor(255, 60, 60), TFT_BLACK);
      snprintf(buf, sizeof(buf), "TRAP TRIGGERED! POISON DAMAGED!");
    } else {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      snprintf(buf, sizeof(buf), "FOUND: +%d GOLD! %s", chestGoldObtained, chestItemObtained);
    }
  }
  img.drawString(buf, 14, TFT_HEIGHT - 17, 1);

  xSemaphoreGive(dataMutex);
}

// コレクション（所持アイテム一覧）画面の描画
void renderCollectionView() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  img.fillSprite(myColor(8, 12, 22));
  img.drawRect(2, 2, TFT_WIDTH - 4, TFT_HEIGHT - 4, myColor(255, 215, 0));
  img.drawRect(4, 4, TFT_WIDTH - 8, TFT_HEIGHT - 8, myColor(0, 200, 255));

  img.fillRect(8, 6, TFT_WIDTH - 16, 24, TFT_BLACK);
  img.drawRect(8, 6, TFT_WIDTH - 16, 24, myColor(255, 215, 0));

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
  img.setTextSize(1);
  img.drawString("=== HACK & SLASH LOOT COLLECTION ===", TFT_WIDTH / 2, 17, 1);

  for (int i = 0; i < collectionCount; i++) {
    int cardY = 34 + i * 21;
    uint16_t cardBorder = TFT_WHITE;
    uint16_t cardText   = TFT_WHITE;

    if (collectionList[i].rarity == RARITY_LEGENDARY) {
      cardBorder = myColor(255, 215, 0);
      cardText   = myColor(255, 220, 40);
    } else if (collectionList[i].rarity == RARITY_EPIC) {
      cardBorder = myColor(220, 80, 255);
      cardText   = myColor(230, 140, 255);
    } else if (collectionList[i].rarity == RARITY_RARE) {
      cardBorder = myColor(0, 180, 255);
      cardText   = myColor(80, 210, 255);
    } else {
      cardBorder = myColor(160, 160, 170);
      cardText   = myColor(200, 200, 200);
    }

    img.fillRect(10, cardY, TFT_WIDTH - 20, 19, TFT_BLACK);
    img.drawRect(10, cardY, TFT_WIDTH - 20, 19, cardBorder);

    img.setTextDatum(textdatum_t::middle_left);
    img.setTextColor(cardText, TFT_BLACK);
    img.drawString(collectionList[i].name, 14, cardY + 9, 1);

    int maxAtkInList = 0, maxDefInList = 0;
    for (int k = 0; k < collectionCount; k++) {
      if (collectionList[k].atkBonus > maxAtkInList) maxAtkInList = collectionList[k].atkBonus;
      if (collectionList[k].defBonus > maxDefInList) maxDefInList = collectionList[k].defBonus;
    }
    bool isBest = (collectionList[i].atkBonus == maxAtkInList && maxAtkInList > 0) || (collectionList[i].defBonus == maxDefInList && maxDefInList > 0);

    char statBuf[32];
    if (isBest) {
      snprintf(statBuf, sizeof(statBuf), "+%d [EQUIPPED]", max(collectionList[i].atkBonus, collectionList[i].defBonus));
      img.setTextDatum(textdatum_t::middle_right);
      img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
    } else {
      snprintf(statBuf, sizeof(statBuf), "+%d ATK +%d DEF", collectionList[i].atkBonus, collectionList[i].defBonus);
      img.setTextDatum(textdatum_t::middle_right);
      img.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    img.drawString(statBuf, TFT_WIDTH - 14, cardY + 9, 1);
  }

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
  img.drawString("[ TAP SCREEN TO RETURN TO MAZE ]", TFT_WIDTH / 2, TFT_HEIGHT - 9, 1);

  xSemaphoreGive(dataMutex);
}

// 戦闘画面の描画
void renderBattleScreen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  unsigned long pElapsed = millis() - phaseTimer;
  bool monsterFlashing = millis() < monsterHitFlashUntil;
  bool heroFlashing    = millis() < heroHitFlashUntil;

  uint16_t bgCol = isRareEncounter ? myColor(40, 30, 5) : myColor(10, 10, 25);
  uint16_t frameCol = isRareEncounter ? myColor(255, 215, 0) : ((dungeonFloor % 5 == 0) ? myColor(240, 40, 40) : TFT_WHITE);

  if (heroFlashing && battlePhase == BP_MONSTER_COUNTER && (millis() / 70) % 2 == 0) {
    frameCol = myColor(255, 40, 40);
  }

  img.fillSprite(bgCol);

  if (isBossEncounter || isRareEncounter) {
    uint16_t partCol = isBossEncounter ? myColor(255, 90, 20) : myColor(255, 225, 120);
    for (int i = 0; i < 7; i++) {
      float t = (millis() / 900.0f) + i * 0.9f;
      int px = TFT_WIDTH / 2 + (int)(sinf(t * 1.3f + i) * (70 + i * 6));
      int py = (TFT_HEIGHT - 20) - (int)fmodf(t * 40.0f + i * 23.0f, (float)(TFT_HEIGHT - 60));
      if (py > 40 && py < TFT_HEIGHT - 34) img.fillCircle(px, py, 1 + (i % 2), partCol);
    }
  }

  img.fillRect(8, 6, TFT_WIDTH - 16, 32, TFT_BLACK);
  img.drawRect(8, 6, TFT_WIDTH - 16, 32, frameCol);

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(TFT_WHITE, TFT_BLACK);
  img.setTextSize(1);

  char buf[64];
  if (battlePhase == BP_INTRO) {
    const char* mPrefix = (dungeonFloor >= 10) ? "INFERNAL" : ((dungeonFloor >= 6) ? "ABYSSAL" : ((dungeonFloor >= 3) ? "ENRAGED" : "LESSER"));
    if (isBossEncounter)         snprintf(buf, sizeof(buf), "[!] B%dF BOSS: %s DEMON LORD! [!]", dungeonFloor, mPrefix);
    else if (isRareEncounter)    snprintf(buf, sizeof(buf), "* RARE GOLD SLIME APPEARED! *");
    else if (monsterType == 0)   snprintf(buf, sizeof(buf), "%s SHADOW DEMON!", mPrefix);
    else if (monsterType == 1)   snprintf(buf, sizeof(buf), "%s ELDRITCH BEAST!", mPrefix);
    else if (monsterType == 2)   snprintf(buf, sizeof(buf), "%s VOID GARGOYLE!", mPrefix);
    else if (monsterType == 3)   snprintf(buf, sizeof(buf), "%s HELLHOUND!", mPrefix);
    else if (monsterType == 8)   snprintf(buf, sizeof(buf), "%s PHOENIX DEMON!", mPrefix);
    else if (monsterType == 9)   snprintf(buf, sizeof(buf), "%s BONE DRAGON LORD!", mPrefix);
    else if (monsterType == 10)  snprintf(buf, sizeof(buf), "%s PURPLE LICH MAGE!", mPrefix);
    else if (monsterType == 11)  snprintf(buf, sizeof(buf), "%s DARK WITCH!", mPrefix);
    else                         snprintf(buf, sizeof(buf), "%s DEMON OVERLORD!", mPrefix);
    img.drawString(buf, TFT_WIDTH / 2, 22, 1);
  } else if (battlePhase == BP_HERO_ATTACK) {
    if (isLimitOverdrive) {
      img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
      snprintf(buf, sizeof(buf), "*** DOUBLE LIMIT OVERDRIVE %d! ***", lastDamageDealt);
    } else if (comboHits > 1) {
      img.setTextColor(myColor(255, 220, 0), TFT_BLACK);
      snprintf(buf, sizeof(buf), "FEVER COMBO x%d! DAMAGE %d!", comboHits, lastDamageDealt);
    } else if (isHealSpell) {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      snprintf(buf, sizeof(buf), ">> HEAL SPELL! RECOVERED +%d HP! <<", healAmountDealt);
    } else if (isSpellAttack) {
      img.setTextColor(TFT_CYAN, TFT_BLACK);
      if (spellType == 0) snprintf(buf, sizeof(buf), ">> FIREBALL! DAMAGE %d! <<", lastDamageDealt);
      else                snprintf(buf, sizeof(buf), ">> LIGHTNING! DAMAGE %d! <<", lastDamageDealt);
    } else if (isCriticalHit) {
      img.setTextColor(TFT_YELLOW, TFT_BLACK);
      snprintf(buf, sizeof(buf), "* CRITICAL STRIKE! DAMAGE %d! *", lastDamageDealt);
    } else {
      snprintf(buf, sizeof(buf), "HERO ATTACKS! (DAMAGE %d)", lastDamageDealt);
    }
    img.drawString(buf, TFT_WIDTH / 2, 22, 1);
  } else if (battlePhase == BP_MONSTER_COUNTER) {
    img.setTextColor(TFT_ORANGE, TFT_BLACK);
    snprintf(buf, sizeof(buf), "MONSTER COUNTERS! DAMAGE %d!", lastMonsterDamage);
    img.drawString(buf, TFT_WIDTH / 2, 22, 1);
  } else if (battlePhase == BP_VICTORY) {
    if (pElapsed < 700) {
      img.setTextColor(TFT_YELLOW, TFT_BLACK);
      snprintf(buf, sizeof(buf), "--- FINISHING BLOW! ENEMY SLAIN! ---");
    } else {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      snprintf(buf, sizeof(buf), "=== VICTORY! +%d EXP +%dG ===", monsterEXP, monsterGold);
    }
    img.drawString(buf, TFT_WIDTH / 2, 22, 1);
  } else { // BP_DEFEAT
    img.setTextColor(myColor(255, 70, 70), TFT_BLACK);
    snprintf(buf, sizeof(buf), "*** YOU WERE OVERWHELMED! RETREAT! ***");
    img.drawString(buf, TFT_WIDTH / 2, 22, 1);
  }

  // ターン数の表示
  img.setTextDatum(textdatum_t::top_right);
  img.setTextColor(myColor(160, 160, 180), TFT_BLACK);
  snprintf(buf, sizeof(buf), "T%d", battleTurn);
  img.drawString(buf, TFT_WIDTH - 12, 9, 1);

  int shakeX = (battlePhase == BP_HERO_ATTACK && (isCriticalHit || isSpellAttack || isLimitOverdrive || comboHits > 1)) ? random(-6, 7) : 0;
  int shakeY = (battlePhase == BP_HERO_ATTACK && (isCriticalHit || isSpellAttack || isLimitOverdrive || comboHits > 1)) ? random(-6, 7) : 0;

  if (battlePhase == BP_HERO_ATTACK && isSpellAttack && pElapsed < 260) {
    uint16_t spellFlashCol = (spellType == 0) ? myColor(255, 60, 20) : myColor(0, 240, 255);
    img.fillCircle(TFT_WIDTH / 2 + shakeX, TFT_HEIGHT / 2 + 4 + shakeY, 42, spellFlashCol);
  } else if (battlePhase == BP_HERO_ATTACK && monsterFlashing && (millis() / 60) % 2 == 0) {
    img.fillCircle(TFT_WIDTH / 2 + shakeX, TFT_HEIGHT / 2 + 4 + shakeY, 38, TFT_WHITE);
  } else if (battlePhase != BP_DEFEAT) {
    drawRLEMonsterSprite(&img, TFT_WIDTH / 2 + shakeX, TFT_HEIGHT / 2 + 4 + shakeY, (monsterType == 4) ? 0 : monsterType, 2);
  }

  // 攻撃時の斬撃エフェクトおよびパーティクル描画
  if (millis() < slashEffectUntil) {
    int cx = TFT_WIDTH / 2 + shakeX;
    int cy = TFT_HEIGHT / 2 + 4 + shakeY;
    
    for (int h = 0; h < comboHits; h++) {
      int flip = (h % 2 == 0) ? 1 : -1;
      int x1 = cx + (45 * flip);
      int y1 = cy - 42 + (h * 6);
      int x2 = cx - (45 * flip);
      int y2 = cy + 42 - (h * 6);

      uint16_t mainCol  = (comboHits >= 3) ? myColor(255, 220, 30) : (isCriticalHit ? myColor(255, 230, 80) : TFT_WHITE);
      uint16_t glowCol  = (comboHits >= 3) ? myColor(255, 100, 0)  : (isCriticalHit ? myColor(255, 140, 20) : myColor(0, 200, 255));

      img.drawLine(x1 - 2, y1, x2 - 2, y2, glowCol);
      img.drawLine(x1 + 2, y1, x2 + 2, y2, glowCol);
      img.drawLine(x1 - 1, y1, x2 - 1, y2, mainCol);
      img.drawLine(x1 + 1, y1, x2 + 1, y2, mainCol);
      img.drawLine(x1, y1, x2, y2, TFT_WHITE);

      for (int p = 0; p < 5; p++) {
        float t = (p / 4.0f);
        int px = x1 + (int)((x2 - x1) * t) + random(-10, 11);
        int py = y1 + (int)((y2 - y1) * t) + random(-10, 11);
        img.fillCircle(px, py, (p % 2 == 0) ? 2 : 1, (p % 2 == 0) ? TFT_WHITE : glowCol);
      }
    }
  }

  if (battlePhase == BP_HERO_ATTACK && pElapsed < 500) {
    int rise = (int)(pElapsed / 14);
    img.setTextDatum(textdatum_t::middle_center);
    img.setTextColor(isLimitOverdrive ? myColor(255, 215, 0) : (isCriticalHit || isSpellAttack ? TFT_YELLOW : TFT_WHITE), bgCol);
    img.setTextSize(isLimitOverdrive || isCriticalHit || isSpellAttack ? 2 : 1);
    snprintf(buf, sizeof(buf), "-%d", lastDamageDealt);
    img.drawString(buf, TFT_WIDTH / 2 + 30, (TFT_HEIGHT / 2 - 20) - rise, 1);
    img.setTextSize(1);
  }
  if (battlePhase == BP_MONSTER_COUNTER && pElapsed < 500) {
    int rise = (int)(pElapsed / 14);
    img.setTextDatum(textdatum_t::middle_center);
    img.setTextColor(myColor(255, 90, 90), bgCol);
    snprintf(buf, sizeof(buf), "-%d", lastMonsterDamage);
    img.drawString(buf, TFT_WIDTH / 2 - 60, (TFT_HEIGHT - 40) - rise, 1);
  }

  // 画面下部ステータス表示
  int boxY = TFT_HEIGHT - 28;
  img.fillRect(8, boxY, TFT_WIDTH - 16, 22, TFT_BLACK);
  img.drawRect(8, boxY, TFT_WIDTH - 16, 22, frameCol);

  img.setTextDatum(textdatum_t::middle_left);
  img.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "HP:%d/%d ATK:%d DEF:%d G:%d", hero.hp, hero.maxHP, getHeroTotalATK(), getHeroTotalDEF(), hero.gold);
  img.drawString(buf, 14, boxY + 11, 1);

  // リミットゲージ描画
  int barX = TFT_WIDTH - 82;
  int safeGauge = constrain(limitGauge, 0, 100);
  bool isFull = (safeGauge >= 100);
  
  uint16_t gaugeBorderCol = isFull ? myColor(255, 215, 0) : myColor(0, 180, 220);
  uint16_t gaugeFillCol   = isFull ? myColor(255, 215, 0) : myColor(0, 200, 255);

  img.drawRect(barX, boxY + 4, 70, 14, gaugeBorderCol);
  img.fillRect(barX + 1, boxY + 5, 68, 12, TFT_BLACK);

  int fillW = (int)(66.0f * (safeGauge / 100.0f));
  if (fillW > 0) {
    img.fillRect(barX + 2, boxY + 6, fillW, 10, gaugeFillCol);
  }

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextColor(isFull ? TFT_BLACK : TFT_WHITE, isFull ? myColor(255, 215, 0) : TFT_TRANSPARENT);
  snprintf(buf, sizeof(buf), isFull ? "LIMIT!" : "%d%%", safeGauge);
  img.drawString(buf, barX + 35, boxY + 11, 1);

  xSemaphoreGive(dataMutex);
}

// ============================================================================
//  ゲームオーバー画面（ドット絵画像描画・送還カウントダウン）
// ============================================================================
void drawConvertedGameOverImage(int dstX, int dstY) {
  for (int y = 0; y < GAME_OVER_IMG_H; y++) {
    int x = 0;
    while (x < GAME_OVER_IMG_W) {
      uint16_t color = pgm_read_word(&gameOverImagePixelData[y * GAME_OVER_IMG_W + x]);
      int run = 1;
      while (x + run < GAME_OVER_IMG_W && pgm_read_word(&gameOverImagePixelData[y * GAME_OVER_IMG_W + x + run]) == color) {
        run++;
      }
      // ピクセルデータの描画
      img.fillRect(dstX + x, dstY + y, run, 1, color);
      x += run;
    }
  }
}

void renderGameOverScreen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  unsigned long elapsed = millis() - battleTimer;

  // ゲームオーバー画像描画
  drawConvertedGameOverImage(0, 0);

  // 下部テキストエリア（文字被り防止用の黒帯領域）
  img.fillRect(0, TFT_HEIGHT - 38, TFT_WIDTH, 38, TFT_BLACK);
  img.drawFastHLine(0, TFT_HEIGHT - 38, TFT_WIDTH, myColor(180, 40, 40));

  // 累計死亡回数の表示
  img.setTextDatum(textdatum_t::middle_center);
  img.setTextSize(1);

  char deathBuf[48];
  snprintf(deathBuf, sizeof(deathBuf), "FALLEN HEROES: %d", totalDeaths);
  img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
  img.drawString(deathBuf, TFT_WIDTH / 2, TFT_HEIGHT - 26, 1);

  // 送還カウントダウン
  if (elapsed > 1800) {
    int remaining = max(0, (int)((3500 - (int)elapsed) / 1000) + 1);
    char buf[64];
    snprintf(buf, sizeof(buf), "RETURNING TO B1F... (%d)", remaining);
    img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
    img.drawString(buf, TFT_WIDTH / 2, TFT_HEIGHT - 10, 1);
  }

  xSemaphoreGive(dataMutex);
}


// ============================================================================
//  墓画面（遺品回収・祈りメッセージ）
// ============================================================================
const char* prayerMessages[] = {
  "May your soul rest in eternal peace.",
  "Your courage shall never be forgotten.",
  "The dungeon remembers the fallen hero.",
  "Even in death, your legend lives on.",
  "Rest now, brave warrior of the deep."
};

void renderGraveScreen() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);

  unsigned long elapsed = millis() - graveTimer;
  bool isRolling = (elapsed < 1000);
  float rotA = elapsed * 0.009f;

  // 背景描画
  img.fillSprite(myColor(8, 5, 18));
  img.fillRect(8, 6, TFT_WIDTH - 16, 32, TFT_BLACK);
  img.drawRect(8, 6, TFT_WIDTH - 16, 32, myColor(120, 120, 160));

  img.setTextDatum(textdatum_t::middle_center);
  img.setTextSize(1);

  if (isRolling) {
    img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
    img.drawString("--- ROLLING D20 DESTINY DICE FOR GRAVE... ---", TFT_WIDTH / 2, 22, 1);
  } else {
    if (graveD20Result == 20) {
      img.setTextColor(myColor(255, 215, 0), TFT_BLACK);
      img.drawString("★ D20 ROLL: 20 NAT 20 PERFECT INHERITANCE! ★", TFT_WIDTH / 2, 22, 1);
    } else if (graveD20Result == 1) {
      img.setTextColor(myColor(255, 60, 60), TFT_BLACK);
      img.drawString("💀 D20 ROLL: 1 FUMBLE GRAVE COLLAPSED... 💀", TFT_WIDTH / 2, 22, 1);
    } else {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      char d20Buf[48];
      snprintf(d20Buf, sizeof(d20Buf), "=== D20 ROLL RESULT: %d RECOVERED %d / %d ===", graveD20Result, graveRecoveredCount, graveItemCount);
      img.drawString(d20Buf, TFT_WIDTH / 2, 22, 1);
    }
  }

  // 3D 20面ダイスの回転アニメーション描画
  draw3DD20Dice(TFT_WIDTH * 3 / 4 - 10, TFT_HEIGHT / 2 + 4, rotA, graveD20Result, isRolling);

  // 回収アイテムおよび自動装備リストの表示
  if (!isRolling) {
    img.setTextDatum(textdatum_t::middle_left);
    if (graveRecoveredCount > 0 && (graveWeaponId > 0 || graveArmorId > 0)) {
      img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
      img.drawString("AUTO-EQUIPPED: INHERITED GEAR!", 14, TFT_HEIGHT / 2 - 20, 1);
    }

    for (int i = 0; i < min(graveRecoveredCount, 3); i++) {
      uint16_t rarCol = (graveItems[i].rarity == RARITY_LEGENDARY) ? myColor(255,140,0) :
                        (graveItems[i].rarity == RARITY_EPIC)      ? myColor(180,0,255) :
                        (graveItems[i].rarity == RARITY_RARE)       ? myColor(0,160,255) : TFT_WHITE;
      img.setTextColor(rarCol, TFT_BLACK);
      img.drawString(graveItems[i].name, 14, TFT_HEIGHT / 2 - 4 + i * 14, 1);
    }
  }

  // 祈りメッセージおよびカウントダウン表示
  img.fillRect(8, TFT_HEIGHT - 28, TFT_WIDTH - 16, 22, TFT_BLACK);
  img.setTextDatum(textdatum_t::middle_left);
  int msgIdx = (graveDeepestFloor + graveItemCount) % 5;
  img.setTextColor(myColor(180, 220, 255), TFT_BLACK);
  img.drawString(prayerMessages[msgIdx], 14, TFT_HEIGHT - 17, 1);

  if (elapsed > 2000) {
    int rem = max(0, (int)((4000 - (int)elapsed) / 1000) + 1);
    char contBuf[32];
    snprintf(contBuf, sizeof(contBuf), "(%d)", rem);
    img.setTextDatum(textdatum_t::middle_right);
    img.setTextColor(myColor(0, 220, 255), TFT_BLACK);
    img.drawString(contBuf, TFT_WIDTH - 14, TFT_HEIGHT - 17, 1);
  }

  xSemaphoreGive(dataMutex);
}

void resumeExplore3D() {
  isBossEncounter = false;
  isRareEncounter = false;
  int curX = (int)cam.posX;
  int curY = (int)cam.posY;
  int nextD = decideNextDirection(curX, curY, currentDir);
  if (nextD != currentDir) {
    motionState = TURNING;
    startDir = currentDir;
    endDir = nextD;
    animProgress = 0.0f;
  } else {
    targetGridX = curX + dx[currentDir];
    targetGridY = curY + dy[currentDir];
    startPosX = cam.posX;
    startPosY = cam.posY;
    targetPosX = targetGridX + 0.5f;
    targetPosY = targetGridY + 0.5f;
    motionState = MOVING_FORWARD;
    animProgress = 0.0f;
  }
  appMode = MODE_EXPLORE_3D;
}

void core0Task(void * pvParameters) {
  for(;;) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // BOOTボタン (GPIO 0) 押下によるテスト用死亡処理
    // 連続検知を防止するための時間ガードを追加
    static unsigned long lastBootBtnDeath = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW && appMode != MODE_GAME_OVER && appMode != MODE_GRAVE_SCREEN
        && millis() - lastBootBtnDeath > 1000) {
      lastBootBtnDeath = millis();
      triggerHeroDeath();
    }

    if (appMode == MODE_EXPLORE_3D) {
      if (motionState == MOVING_FORWARD) {
        animProgress += 0.045f;
        if (animProgress >= 1.0f) {
          animProgress = 1.0f;
          cam.posX = targetPosX;
          cam.posY = targetPosY;
          int curX = (int)cam.posX;
          int curY = (int)cam.posY;
          
          totalStepsTaken++;

          // 階段(2)に到達した場合、次の階層へ移動
          if (worldMap[curY][curX] == 2) {
            dungeonFloor++;
            bossDefeatedOnFloor = false;
            generateRandomMaze();

            // プレイヤー初期位置(1.5, 1.5)へ更新
            cam.posX = 1.5f;
            cam.posY = 1.5f;
            currentDir = 2; // Facing South
            setCameraDirection(M_PI / 2.0f);
            targetGridX = 1;
            targetGridY = 2;
            startPosX = 1.5f; startPosY = 1.5f;
            targetPosX = 1.5f; targetPosY = 2.5f;
            motionState = MOVING_FORWARD;
            animProgress = 0.0f;

            // 階層変更時にセーブ実行
            saveGame();
          }
          // 毒トラップ(4)に移動した場合
          else if (worldMap[curY][curX] == 4) {
            trapDamageTaken = (int)max((long)3, (long)(hero.maxHP * 0.12f));
            hero.hp = max(1, hero.hp - trapDamageTaken);
            trapFlashUntil = millis() + 350;
          }
          // 墓タイル(3)を踏んだ場合の遺品回収処理
          else if (worldMap[curY][curX] == 3 && graveActive) {
            graveD20Result = random(1, 21);
            graveRecoveredCount = 0;
            if (graveItemCount == 0) {
              graveRecoveredCount = 0; // 遺品0個の場合は回収0
            } else if (graveD20Result == 20) {
              graveRecoveredCount = graveItemCount; // 全回収
            } else if (graveD20Result == 1) {
              graveRecoveredCount = 0; // 墓崩壊
            } else {
              graveRecoveredCount = max(1, graveItemCount * graveD20Result / 20);
            }
            // 回収アイテムをコレクションに追加＆前世の強力な装備を自動装着！
            for (int i = 0; i < graveRecoveredCount && collectionCount < 6; i++) {
              collectionList[collectionCount++] = graveItems[i];
            }
            if (graveRecoveredCount > 0) {
              if (graveWeaponId > hero.weaponId) hero.weaponId = graveWeaponId;
              if (graveArmorId > hero.armorId)   hero.armorId  = graveArmorId;
            }
            // 墓の回収処理（マップ上の墓を消去して画面モード変更）
            graveActive = false;
            worldMap[curY][curX] = 0; // マップ上の墓を消去
            graveTimer = millis();
            appMode = MODE_GRAVE_SCREEN;

            // 回収確定時にNVSへ保存
            saveGrave();
          }

          if (totalStepsTaken % 3 == 0 && hero.hp < hero.maxHP) {
            hero.hp = min(hero.maxHP, hero.hp + 1 + hero.level / 4);
          }
          if (totalStepsTaken % 5 == 0 && hero.mp < hero.maxMP) {
            hero.mp = min(hero.maxMP, hero.mp + 1);
          }

          // ランダムエンカウント判定
          if (appMode == MODE_EXPLORE_3D) {
            int roll = random(0, 100);
            if (roll < 22) { // Event Encounter
              appMode = MODE_BATTLE_FLASH;
              battleTimer = millis();
              
              if (roll < 6) { // 6% Merchant Encounter with Healing & Upgrade Rules
              appMode = MODE_SHOP_SCREEN;
              
              // [優先ルール1: HP回復ポーション] (HP80%未満 & 35G以上)
              if (hero.hp < (hero.maxHP * 4 / 5) && hero.gold >= 35) {
                hero.gold -= 35;
                hero.hp = hero.maxHP;
                snprintf(shopActionMessage, sizeof(shopActionMessage), "BOUGHT: HP POTION (-35G)");
                snprintf(shopDetailMessage, sizeof(shopDetailMessage), "HP FULLY RECOVERED!");
              }
              // [優先ルール2: MP回復エーテル] (MP60%未満 & 25G以上)
              else if (hero.mp < (hero.maxMP * 3 / 5) && hero.gold >= 25) {
                hero.gold -= 25;
                hero.mp = hero.maxMP;
                snprintf(shopActionMessage, sizeof(shopActionMessage), "BOUGHT: MP ETHER (-25G)");
                snprintf(shopDetailMessage, sizeof(shopDetailMessage), "MP FULLY RECOVERED!");
              }
              // [優先ルール3: 武器購入/強化]
              else if (hero.weaponId < 2 && hero.gold >= weaponDatabase[hero.weaponId + 1].price && weaponDatabase[hero.weaponId + 1].price > 0) {
                int nextWep = hero.weaponId + 1;
                hero.gold -= weaponDatabase[nextWep].price;
                hero.weaponId = nextWep;
                snprintf(shopActionMessage, sizeof(shopActionMessage), "WEAPON UPGRADED!");
                snprintf(shopDetailMessage, sizeof(shopDetailMessage), "+%d ATK (%s)", weaponDatabase[nextWep].atkBonus, weaponDatabase[nextWep].name);
              }
              // [優先ルール4: 防具購入/強化]
              else if (hero.armorId < 2 && hero.gold >= armorDatabase[hero.armorId + 1].price && armorDatabase[hero.armorId + 1].price > 0) {
                int nextArm = hero.armorId + 1;
                hero.gold -= armorDatabase[nextArm].price;
                hero.armorId = nextArm;
                snprintf(shopActionMessage, sizeof(shopActionMessage), "ARMOR UPGRADED!");
                snprintf(shopDetailMessage, sizeof(shopDetailMessage), "+%d DEF (%s)", armorDatabase[nextArm].defBonus, armorDatabase[nextArm].name);
              }
              // [買い物不要 / ゴールド不足]
              else {
                snprintf(shopActionMessage, sizeof(shopActionMessage), "MERCHANT WELCOME!");
                snprintf(shopDetailMessage, sizeof(shopDetailMessage), "\"STAY SAFE, HERO!\"");
              }
            } else if (roll < 7) { // 3% Treasure Chest Encounter with 3D D20 Dice Roll!
              appMode = MODE_CHEST_SCREEN;
              battleTimer = millis();
              
              d20RollResult = random(1, 21);

              if (d20RollResult == 20) {
                chestGoldObtained = random(350, 800);
                hero.gold += chestGoldObtained;
                chestItemObtained = "LEGENDARY RELIC";
                dropLootItem();
              } else if (d20RollResult == 1) {
                chestGoldObtained = 0;
                int trapDmg = (int)(hero.maxHP * 0.15f);
                hero.hp = max(1, hero.hp - trapDmg);
                chestItemObtained = "TRAP EXPLOSION";
              } else {
                chestGoldObtained = (int)(random(60, 180) * (d20RollResult / 10.0f + 0.5f));
                hero.gold += chestGoldObtained;
                chestItemObtained = "RARE LOOT ITEM";
                dropLootItem();
              }
            } else if ((dungeonFloor % 5 == 0) && !bossDefeatedOnFloor && random(0, 100) < 40) {
              isBossEncounter = true;
              isRareEncounter = false;
              monsterType = 5;
              computeMonsterStats(5);
            } else if (random(0, 100) < 6) {
              isBossEncounter = false;
              isRareEncounter = true;
              monsterType = 4;
              computeMonsterStats(4);
            } else {
              isBossEncounter = false;
              isRareEncounter = false;
              // 階層に応じたモンスターの出現設定
              int rTile = random(0, 100);
              if (dungeonFloor <= 2) {
                monsterType = (rTile < 40) ? 0 : ((rTile < 75) ? 11 : 1);  // B1-2: Shadow Demon, Assassin, Eldritch
              } else if (dungeonFloor <= 5) {
                monsterType = (rTile < 35) ? 1 : ((rTile < 70) ? 8 : 2);   // B3-5: Eldritch, Phoenix, Gargoyle
              } else if (dungeonFloor <= 9) {
                monsterType = (rTile < 35) ? 2 : ((rTile < 70) ? 10 : 3);  // B6-9: Gargoyle, Lich, Hellhound
              } else {
                // ※通常モンスター枠で専用ボス(monsterType=5)が出現しないよう設定
                monsterType = (rTile < 35) ? 3 : ((rTile < 70) ? 9 : 10);  // B10+: Hellhound, Bone Dragon, Purple Lich
              }
              computeMonsterStats(monsterType);
            }

            battleStep = 0;
            battlePhase = BP_INTRO;
            battleTurn = 1;
            heroWasDefeated = false;
            monsterHitFlashUntil = 0;
            heroHitFlashUntil = 0;
          }
        }
        
        if (appMode == MODE_EXPLORE_3D) {
          int nextD = decideNextDirection(curX, curY, currentDir);

            if (nextD != currentDir) {
              motionState = TURNING;
              startDir = currentDir;
              endDir = nextD;
              animProgress = 0.0f;
            } else {
              targetGridX = curX + dx[currentDir];
              targetGridY = curY + dy[currentDir];
              startPosX = cam.posX;
              startPosY = cam.posY;
              targetPosX = targetGridX + 0.5f;
              targetPosY = targetGridY + 0.5f;
              animProgress = 0.0f;
            }
          }
        } else {
          cam.posX = startPosX + (targetPosX - startPosX) * animProgress;
          cam.posY = startPosY + (targetPosY - startPosY) * animProgress;
        }
      } else if (motionState == TURNING) {
        animProgress += 0.05f;
        if (animProgress >= 1.0f) {
          animProgress = 1.0f;
          currentDir = endDir;
          float finalAngle = currentDir * M_PI_2 - M_PI_2;
          setCameraDirection(finalAngle);

          int curX = (int)cam.posX;
          int curY = (int)cam.posY;
          targetGridX = curX + dx[currentDir];
          targetGridY = curY + dy[currentDir];
          startPosX = cam.posX;
          startPosY = cam.posY;
          targetPosX = targetGridX + 0.5f;
          targetPosY = targetGridY + 0.5f;

          motionState = MOVING_FORWARD;
          animProgress = 0.0f;
        } else {
          float startAngle = startDir * M_PI_2 - M_PI_2;
          float endAngle = endDir * M_PI_2 - M_PI_2;

          float diff = endAngle - startAngle;
          if (diff > M_PI)  diff -= 2.0f * M_PI;
          if (diff < -M_PI) diff += 2.0f * M_PI;

          float curAngle = startAngle + diff * animProgress;
          setCameraDirection(curAngle);
        }
      } else if (motionState == IDLE) {
        motionState = MOVING_FORWARD;
        animProgress = 0.0f;
      }
    } else if (appMode == MODE_SHOP_SCREEN || appMode == MODE_CHEST_SCREEN) {
      if (millis() - battleTimer > 2500) {
        resumeExplore3D();
      }
    } else if (appMode == MODE_BATTLE_FLASH) {
      if (millis() - battleTimer > 400) {
        appMode = MODE_BATTLE_SCREEN;
        phaseTimer = millis();
        battlePhase = BP_INTRO;
        battleTurn = 1;
      }
    } else if (appMode == MODE_BATTLE_SCREEN) {
      unsigned long pElapsed = millis() - phaseTimer;

      if (battlePhase == BP_INTRO) {
        if (pElapsed > 1100) {
          battlePhase = BP_HERO_ATTACK;
          phaseTimer = millis();
          resolveHeroAttack();
        }
      } else if (battlePhase == BP_HERO_ATTACK) {
        if (pElapsed > 1100) {
          if (monsterHP <= 0) {
            battlePhase = BP_VICTORY;
            phaseTimer = millis();
          } else {
            battlePhase = BP_MONSTER_COUNTER;
            phaseTimer = millis();
            resolveMonsterCounter();
          }
        }
      } else if (battlePhase == BP_MONSTER_COUNTER) {
        if (pElapsed > 1100) {
          if (hero.hp <= 0) {
            heroWasDefeated = true;
            battlePhase = BP_DEFEAT;
            phaseTimer = millis();
          } else {
            battleTurn++;
            battlePhase = BP_HERO_ATTACK;
            phaseTimer = millis();
            resolveHeroAttack();
          }
        }
      } else if (battlePhase == BP_VICTORY) {
        if (pElapsed > 1800) {
          if (isBossEncounter) {
            bossDefeatedOnFloor = true;
            saveGame(); // ボス撃破直後にNVSへフラグを永続保存！
          }
          hero.exp += monsterEXP;
          hero.gold += monsterGold;

          if (hero.exp >= hero.nextEXP) {
            hero.level++;
            hero.maxHP += 25;
            hero.hp = hero.maxHP;
            hero.maxMP += 10;
            hero.mp = hero.maxMP;
            hero.baseATK += 5;
            hero.baseDEF += 3;
            hero.nextEXP += 90 * hero.level;
            appMode = MODE_LEVEL_UP_FLASH;
            battleTimer = millis();
          } else {
            resumeExplore3D();
          }
        }
      } else if (battlePhase == BP_DEFEAT) {
        if (pElapsed > 1400) {
          triggerHeroDeath();
        }
      }
    } else if (appMode == MODE_GAME_OVER) {
      if (millis() - battleTimer > 3500) {
        // ゲームオーバー後の初期化（レベル1リセット、墓からのデータ引き継ぎ準備）
        dungeonFloor = 1;
        totalStepsTaken = 0;
        bossDefeatedOnFloor = false;
        isBossEncounter = false;
        isRareEncounter = false;
        hero.level = 1;
        hero.exp = 0;
        hero.nextEXP = 100;
        hero.maxHP = 50;
        hero.hp = 50;
        hero.maxMP = 10;
        hero.mp = 10;
        hero.baseATK = 12;
        hero.baseDEF = 8;
        hero.weaponId = 0;
        hero.armorId = 0;
        cam.posX = 1.5f; cam.posY = 1.5f;
        cam.dirX = 0.0f; cam.dirY = 1.0f;
        cam.planeX = -0.66f; cam.planeY = 0.0f;
        currentDir = 2;
        targetPosX = 1.5f; targetPosY = 2.5f;
        startPosX = 1.5f; startPosY = 1.5f;
        collectionCount = 0;
        limitGauge = 0;
        loadGrave();
        generateRandomMaze();
        resumeExplore3D();
      }
    } else if (appMode == MODE_LEVEL_UP_FLASH) {
      if (millis() - battleTimer > 1200) {
        resumeExplore3D();
      }
    } else if (appMode == MODE_GRAVE_SCREEN) {
      // 4秒経過後に自動で探索画面へ復帰
      if (millis() - graveTimer > 4000) {
        resumeExplore3D();
      }
    }

    xSemaphoreGive(dataMutex);
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// 3Dレイキャスティング描画処理
void renderRaycast3D() {
  if (!vignetteLUTInited) initVignetteLUT();
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  
  const BiomeTheme& biome = getCurrentBiome();

  // 時間経過による松明のゆらめく環境光
  float torchFlicker = 1.0f + 0.10f * sinf(millis() * 0.008f) + 0.05f * cosf(millis() * 0.018f);

  int headBobOffset = 0;
  if (motionState == MOVING_FORWARD) {
    headBobOffset = (int)(sinf(animProgress * M_PI * 2.0f) * 3.5f);
  }
  int horizonY = TFT_HEIGHT / 2 + headBobOffset;

  for (int y = 0; y < horizonY; y++) {
    float ceilShade = (float)y / horizonY * 0.4f;
    img.drawFastHLine(0, y, TFT_WIDTH, fadeColor(biome.ceilR, biome.ceilG, biome.ceilB, ceilShade));
  }
  for (int y = horizonY; y < TFT_HEIGHT; y++) {
    float floorShade = 1.0f - (float)(y - horizonY) / (TFT_HEIGHT - horizonY) * 0.5f;
    uint16_t fCol = fadeColor(biome.floorR, biome.floorG, biome.floorB, floorShade);
    img.drawFastHLine(0, y, TFT_WIDTH, fCol);
  }

  float zBuffer[TFT_WIDTH];

  for (int x = 0; x < TFT_WIDTH; x++) {
    float cameraX = 2.0f * x / (float)TFT_WIDTH - 1.0f;
    float rayDirX = cam.dirX + cam.planeX * cameraX;
    float rayDirY = cam.dirY + cam.planeY * cameraX;

    int mapX = (int)cam.posX;
    int mapY = (int)cam.posY;

    float sideDistX, sideDistY;
    float deltaDistX = (rayDirX == 0) ? 1e30f : fabsf(1.0f / rayDirX);
    float deltaDistY = (rayDirY == 0) ? 1e30f : fabsf(1.0f / rayDirY);
    float perpWallDist;

    int stepX, stepY;
    int hit = 0;
    int side = 0;

    if (rayDirX < 0) {
      stepX = -1;
      sideDistX = (cam.posX - mapX) * deltaDistX;
    } else {
      stepX = 1;
      sideDistX = (mapX + 1.0f - cam.posX) * deltaDistX;
    }
    if (rayDirY < 0) {
      stepY = -1;
      sideDistY = (cam.posY - mapY) * deltaDistY;
    } else {
      stepY = 1;
      sideDistY = (mapY + 1.0f - cam.posY) * deltaDistY;
    }

    while (hit == 0) {
      if (sideDistX < sideDistY) {
        sideDistX += deltaDistX;
        mapX += stepX;
        side = 0;
      } else {
        sideDistY += deltaDistY;
        mapY += stepY;
        side = 1;
      }
      if (mapX >= 0 && mapX < MAP_WIDTH && mapY >= 0 && mapY < MAP_HEIGHT) {
        if (worldMap[mapY][mapX] == 1) hit = 1;
      } else {
        hit = 1;
      }
    }

    if (side == 0) perpWallDist = (sideDistX - deltaDistX);
    else          perpWallDist = (sideDistY - deltaDistY);

    if (perpWallDist < 0.1f) perpWallDist = 0.1f;

    float wallX;
    if (side == 0) wallX = cam.posY + perpWallDist * rayDirY;
    else           wallX = cam.posX + perpWallDist * rayDirX;
    wallX -= floorf(wallX);

    int lineHeight = (int)(TFT_HEIGHT / perpWallDist);
    int drawStart = -lineHeight / 2 + horizonY;
    if (drawStart < 0) drawStart = 0;
    int drawEnd = lineHeight / 2 + horizonY;
    if (drawEnd >= TFT_HEIGHT) drawEnd = TFT_HEIGHT - 1;

    float distShade = 1.0f / (1.0f + perpWallDist * 0.35f);
    if (side == 1) distShade *= 0.72f;

    float vigFactor = vignetteLUT[x] / 255.0f;
    float finalShade = distShade * vigFactor * torchFlicker;

    uint16_t baseBrickColor = fadeColor(biome.wallR, biome.wallG, biome.wallB, finalShade);
    uint16_t darkBrickColor = fadeColor(biome.darkR, biome.darkG, biome.darkB, finalShade);
    uint16_t mortarColor    = fadeColor(biome.mortR, biome.mortG, biome.mortB, finalShade * 0.85f);

    int brickH = max(6, lineHeight / 6);

    zBuffer[x] = perpWallDist;

    for (int y = drawStart; y <= drawEnd; y++) {
      int relY = y - (horizonY - lineHeight / 2);
      int brickRow = relY / brickH;
      bool isHorizontalMortar = (relY % brickH == 0);

      float texXOffset = (abs(brickRow) % 2 == 0) ? 0.0f : 0.5f;
      float shiftedWallX = fmodf(wallX + texXOffset, 1.0f);
      bool isVerticalMortar = (shiftedWallX < 0.09f);

      uint16_t col = (isHorizontalMortar || isVerticalMortar) ? mortarColor :
                     (((relY + (int)(wallX * 20)) % 3 == 0) ? darkBrickColor : baseBrickColor);

      img.drawPixel(x, y, col);
    }

    // ----------------------------------------------------
    //  3D床投影による階段描画（タイルID = 2）
    // ----------------------------------------------------
    for (int y = drawEnd + 1; y < TFT_HEIGHT; y++) {
      float p = y - horizonY;
      if (p <= 0) continue;

      float rowDistance = (0.5f * TFT_HEIGHT) / p;

      float floorX = cam.posX + rowDistance * rayDirX;
      float floorY = cam.posY + rowDistance * rayDirY;

      int cellX = (int)floorX;
      int cellY = (int)floorY;

      if (cellX >= 0 && cellX < MAP_WIDTH && cellY >= 0 && cellY < MAP_HEIGHT && worldMap[cellY][cellX] == 2) {
        float u = floorX - cellX;
        float v = floorY - cellY;

        bool isRim = (u < 0.08f || u > 0.92f || v < 0.08f || v > 0.92f);

        if (isRim) {
          uint16_t rimCol = ((int)(u * 12) + (int)(v * 12)) % 2 == 0 ? myColor(150, 155, 170) : myColor(90, 95, 110);
          float shade = 1.0f / (1.0f + rowDistance * 0.35f);
          img.drawPixel(x, y, fadeColor((rimCol >> 11) << 3, ((rimCol >> 5) & 0x3F) << 2, (rimCol & 0x1F) << 3, shade));
        } else {
          // 階段のステップ描画
          float localV = (v - 0.08f) / 0.84f;

          uint16_t stepCol;
          if (localV < 0.25f)       stepCol = myColor(210, 215, 230); // 1段目 踏み面
          else if (localV < 0.28f)  stepCol = myColor(255, 255, 255); // 1段目 エッジ強調
          else if (localV < 0.50f)  stepCol = myColor(150, 155, 170); // 2段目 踏み面
          else if (localV < 0.53f)  stepCol = myColor(220, 225, 240); // 2段目 エッジ
          else if (localV < 0.75f)  stepCol = myColor(90, 95, 110);   // 3段目 踏み面
          else                      stepCol = myColor(40, 45, 60);    // 深底の陰影

          float shade = 1.0f / (1.0f + rowDistance * 0.35f);
          img.drawPixel(x, y, fadeColor((stepCol >> 11) << 3, ((stepCol >> 5) & 0x3F) << 2, (stepCol & 0x1F) << 3, shade));
        }
      }
    }
  }

  // ----------------------------------------------------
  //  3D墓標（ビルボード）の描画
  // ----------------------------------------------------
  if (graveActive) {
    int gX = -1, gY = -1;
    for (int y = 0; y < MAP_HEIGHT; y++) {
      for (int x = 0; x < MAP_WIDTH; x++) {
        if (worldMap[y][x] == 3) { gX = x; gY = y; break; }
      }
      if (gX != -1) break;
    }

    if (gX != -1) {
      float spriteX = (gX + 0.5f) - cam.posX;
      float spriteY = (gY + 0.5f) - cam.posY;

      float invDet = 1.0f / (cam.planeX * cam.dirY - cam.dirX * cam.planeY);
      float transformX = invDet * (cam.dirY * spriteX - cam.dirX * spriteY);
      float transformY = invDet * (-cam.planeY * spriteX + cam.planeX * spriteY);

      if (transformY > 0.15f) {
        int spriteScreenX = (int)((TFT_WIDTH / 2.0f) * (1.0f + transformX / transformY));
        int spriteSize = (int)fabsf(TFT_HEIGHT / transformY);

        int drawStartY = -spriteSize / 2 + horizonY;
        int drawEndY   =  spriteSize / 2 + horizonY;
        if (drawStartY < 0) drawStartY = 0;
        if (drawEndY >= TFT_HEIGHT) drawEndY = TFT_HEIGHT - 1;

        int drawStartX = -spriteSize / 2 + spriteScreenX;
        int drawEndX   =  spriteSize / 2 + spriteScreenX;
        if (drawStartX < 0) drawStartX = 0;
        if (drawEndX >= TFT_WIDTH) drawEndX = TFT_WIDTH - 1;

        uint16_t crossBrightColor = myColor(180, 185, 200);
        uint16_t crossDarkColor   = myColor(95, 100, 115);
        uint16_t auraColor        = ((millis() / 200) % 2 == 0) ? myColor(0, 220, 255) : myColor(120, 240, 255);

        for (int stripe = drawStartX; stripe <= drawEndX; stripe++) {
          if (stripe >= 0 && stripe < TFT_WIDTH && transformY < zBuffer[stripe]) {
            float texX = (float)(stripe - (-spriteSize / 2 + spriteScreenX)) / spriteSize;

            for (int y = drawStartY; y <= drawEndY; y++) {
              float texY = (float)(y - (-spriteSize / 2 + horizonY)) / spriteSize;

              // 十字架形状の墓標描画
              bool isCrossV    = (texX >= 0.43f && texX <= 0.57f && texY >= 0.20f && texY <= 0.90f);
              bool isCrossH    = (texY >= 0.35f && texY <= 0.49f && texX >= 0.28f && texX <= 0.72f);
              bool isCrossBase = (texX >= 0.32f && texX <= 0.68f && texY >= 0.82f && texY <= 0.94f);
              bool isStoneCross = isCrossV || isCrossH || isCrossBase;

              if (isStoneCross) {
                img.drawPixel(stripe, y, (texX < 0.50f) ? crossBrightColor : crossDarkColor);
              } else if (texX >= 0.22f && texX <= 0.78f && texY >= 0.15f && texY <= 0.95f && ((millis() / 150) % 2 == 0)) {
                img.drawPixel(stripe, y, fadeColor(0, 200, 255, 0.35f)); // 周囲のエフェクト描画
              }
            }
          }
        }
      }
    }
  }



  // ----------------------------------------------------
  //  2Dミニマップ（レーダー）の描画
  // ----------------------------------------------------
  int miniX = TFT_WIDTH - 54;
  int miniY = 6;
  img.drawRect(miniX - 1, miniY - 1, 50, 50, myColor(0, 220, 255));
  img.fillRect(miniX, miniY, 48, 48, myColor(5, 10, 20));

  for (int my = 0; my < MAP_HEIGHT; my++) {
    for (int mx = 0; mx < MAP_WIDTH; mx++) {
      int cellVal = worldMap[my][mx];
      if (cellVal == 1) {
        img.fillRect(miniX + mx * 3, miniY + my * 3, 3, 3, fadeColor(biome.wallR, biome.wallG, biome.wallB, 0.7f));
      } else if (cellVal == 2) {
        img.fillRect(miniX + mx * 3, miniY + my * 3, 3, 3, myColor(220, 40, 240));
      } else if (cellVal == 3) {
        // 墓タイル: 点滅白十字
        if ((millis() / 400) % 2 == 0) {
          int gx = miniX + mx * 3 + 1;
          int gy = miniY + my * 3;
          img.drawPixel(gx, gy,     TFT_WHITE);
          img.drawPixel(gx, gy + 1, TFT_WHITE);
          img.drawPixel(gx, gy + 2, TFT_WHITE);
          img.drawPixel(gx - 1, gy + 1, TFT_WHITE);
          img.drawPixel(gx + 1, gy + 1, TFT_WHITE);
        }
      }
    }
  }

  int px = miniX + (int)(cam.posX * 3.0f);
  int py = miniY + (int)(cam.posY * 3.0f);
  img.fillCircle(px, py, 2, TFT_GREEN);
  img.drawLine(px, py, px + (int)(cam.dirX * 5.0f), py + (int)(cam.dirY * 5.0f), TFT_YELLOW);

  if (millis() < trapFlashUntil) {
    img.drawRect(2, 2, TFT_WIDTH - 4, TFT_HEIGHT - 4, myColor(255, 40, 40));
    img.setTextDatum(textdatum_t::middle_center);
    img.setTextColor(myColor(255, 60, 60), TFT_BLACK);
    char buf[64];
    snprintf(buf, sizeof(buf), ">> POISON TRAP! DAMAGE -%d HP <<", trapDamageTaken);
    img.drawString(buf, TFT_WIDTH / 2, TFT_HEIGHT / 2, 1);
  }

  xSemaphoreGive(dataMutex);
}

// 上部HUD（ステータス・現在階層等）の描画
void drawHUD() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  if (now - lastClockTick >= 1000) {
    unsigned long elapsed = (now - lastClockTick) / 1000;
    lastClockTick += elapsed * 1000;
    clockSec += elapsed;
    if (clockSec >= 60) {
      clockMin += clockSec / 60;
      clockSec %= 60;
      if (clockMin >= 60) {
        clockHour += clockMin / 60;
        clockMin %= 60;
        if (clockHour >= 24) clockHour %= 24;
      }
    }
  }

  static unsigned long lastTouchPoll = 0;
  if (now - lastTouchPoll > 30) {
    lastTouchPoll = now;
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      int tx = map(p.x, TOUCH_MIN_X, TOUCH_MAX_X, 0, LCD_WIDTH);
      int ty = map(p.y, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, LCD_HEIGHT);

      static unsigned long lastTouchBtnTime = 0;
      if (now - lastTouchBtnTime > 300) {
        if (ty >= (LCD_HEIGHT - BOT_HUD_HEIGHT)) {
          lastTouchBtnTime = now;
          if (tx >= 165 && tx <= 210) {
            clockHour = (clockHour + 1) % 24;
          } else if (tx >= 215 && tx <= 260) {
            clockMin = (clockMin + 1) % 60;
          } else if (tx >= 265 && tx <= 315) {
            clockSec = 0;
          }
        } else if (ty >= TOP_HUD_HEIGHT && ty < (LCD_HEIGHT - BOT_HUD_HEIGHT)) {
          lastTouchBtnTime = now;
          xSemaphoreTake(dataMutex, portMAX_DELAY);
          if (appMode == MODE_COLLECTION_VIEW) {
            appMode = MODE_EXPLORE_3D;
          } else if (appMode == MODE_EXPLORE_3D) {
            appMode = MODE_COLLECTION_VIEW;
          }
          xSemaphoreGive(dataMutex);
        }
      }
    }
  }

  if (now - lastUpdate < 150) return;
  lastUpdate = now;

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  int curMode = appMode;
  int curFloor = dungeonFloor;
  HeroStatus curHero = hero;
  float cX = cam.posX;
  float cY = cam.posY;
  xSemaphoreGive(dataMutex);

  if (curMode == MODE_LEVEL_UP_FLASH) {
    topHud.fillSprite(myColor(255, 215, 0));
    topHud.setTextDatum(textdatum_t::middle_center);
    topHud.setTextColor(TFT_BLACK, myColor(255, 215, 0));
    topHud.setTextSize(1);
    topHud.drawString("*** LEVEL UP! HP & MP FULL RECOVERED! ***", LCD_WIDTH / 2, 17, 1);
  } else {
    topHud.fillSprite(TFT_BLACK);
    topHud.drawFastHLine(0, TOP_HUD_HEIGHT - 1, LCD_WIDTH, TFT_WHITE);
    
    topHud.setTextDatum(textdatum_t::middle_left);
    topHud.setTextColor(TFT_WHITE, TFT_BLACK);
    topHud.setTextSize(1);
    char buf[64];
    
    snprintf(buf, sizeof(buf), "B%dF | LV:%d HP:%d/%d MP:%d G:%d POS:%02d,%02d", 
             curFloor, curHero.level, curHero.hp, curHero.maxHP, curHero.mp, curHero.gold, (int)cX, (int)cY);
    topHud.drawString(buf, 8, 17, 1);
  }

  topHud.pushSprite(&lcd, 0, 0);

  static int lastSec = -1, lastMin = -1, lastHour = -1;
  if (clockSec != lastSec || clockMin != lastMin || clockHour != lastHour) {
    lastSec = clockSec; lastMin = clockMin; lastHour = clockHour;

    botHud.fillSprite(TFT_BLACK);
    botHud.drawFastHLine(0, 0, LCD_WIDTH, TFT_WHITE);

    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", clockHour, clockMin, clockSec);
    botHud.setTextDatum(textdatum_t::middle_left);
    botHud.setTextColor(TFT_WHITE, TFT_BLACK);
    botHud.setTextSize(2);
    botHud.drawString(buf, 14, 17, 1);

    botHud.setTextSize(1);
    botHud.setTextDatum(textdatum_t::middle_center);

    botHud.drawRoundRect(170, 4, 40, 26, 3, TFT_WHITE);
    botHud.drawString("H+", 190, 17, 1);

    botHud.drawRoundRect(218, 4, 40, 26, 3, TFT_WHITE);
    botHud.drawString("M+", 238, 17, 1);

    botHud.drawRoundRect(266, 4, 44, 26, 3, TFT_WHITE);
    botHud.drawString("00s", 288, 17, 1);

    botHud.pushSprite(&lcd, 0, LCD_HEIGHT - BOT_HUD_HEIGHT);
  }
}

void setup() {
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  lcd.init();
  lcd.setRotation(1); // Fix: Set rotation to 1 for correct vertical orientation on ST7789 CYD
  lcd.setBrightness(255);
  lcd.fillScreen(TFT_BLACK);
  
  img.setColorDepth(16); 
  img.createSprite(TFT_WIDTH, TFT_HEIGHT);

  topHud.setColorDepth(16);
  topHud.createSprite(LCD_WIDTH, TOP_HUD_HEIGHT);

  botHud.setColorDepth(16);
  botHud.createSprite(LCD_WIDTH, BOT_HUD_HEIGHT);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);

  clockHour = 0;
  clockMin  = 0;
  clockSec  = 0;
  lastClockTick = millis();

  dataMutex = xSemaphoreCreateMutex();

  // 起動時にNVSから墓データを読み込み
  loadGrave();

  // 簡易セーブデータが存在する場合は読み込んで再開
  loadGame();
  generateRandomMaze();

  xTaskCreatePinnedToCore(core0Task, "MazeTask", 8000, NULL, 1, &Task1, 0); 
}

void loop() {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  AppMode mode = appMode;
  xSemaphoreGive(dataMutex);

  if (mode == MODE_EXPLORE_3D) {
    renderRaycast3D();
  } else if (mode == MODE_COLLECTION_VIEW) {
    renderCollectionView();
  } else if (mode == MODE_SHOP_SCREEN) {
    renderShopScreen();
  } else if (mode == MODE_CHEST_SCREEN) {
    renderChestScreen();
  } else if (mode == MODE_GAME_OVER) {
    renderGameOverScreen();
  } else if (mode == MODE_GRAVE_SCREEN) {
    renderGraveScreen();
  } else if (mode == MODE_BATTLE_FLASH) {
    if ((millis() / 70) % 2 == 0) img.fillSprite(TFT_WHITE);
    else                          img.fillSprite(TFT_BLACK);
  } else {
    renderBattleScreen();
  }

  lcd.startWrite();
  img.pushSprite(&lcd, 0, OFFSET_Y);
  drawHUD();
  lcd.endWrite();

  vTaskDelay(1 / portTICK_PERIOD_MS);
}
