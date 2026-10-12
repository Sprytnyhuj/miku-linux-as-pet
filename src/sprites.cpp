#include "sprites.h"

#include "config.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QPainter>
#include <QTransform>
#include <algorithm>
#include <cstdio>
#include <vector>

const Anim &Sprites::anim(const QString &name) const
{
    auto it = anims.constFind(name);
    if (it == anims.constEnd())
        it = anims.constFind(QStringLiteral("stand"));
    return it.value();
}

void Sprites::buildMirrored()
{
    const QTransform flip = QTransform().scale(-1, 1);
    for (Anim &a : anims) {
        if (a.mirrored.size() == a.frames.size())
            continue;
        a.mirrored.clear();
        for (const QPixmap &pm : std::as_const(a.frames))
            a.mirrored << pm.transformed(flip);
    }
}

QString findShimejiDir(const QString &root)
{
    if (!QFileInfo(root).isDir())
        return {};
    QStringList hits;
    QDirIterator it(root, {QStringLiteral("shime1.png")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        hits << QFileInfo(it.next()).absolutePath();
    if (hits.isEmpty())
        return {};
    // Płytszy folder wygrywa, przy remisie kolejność alfabetyczna.
    std::sort(hits.begin(), hits.end(), [](const QString &a, const QString &b) {
        const int da = a.count(QLatin1Char('/')), db = b.count(QLatin1Char('/'));
        return da != db ? da < db : a < b;
    });
    return hits.first();
}

namespace {

// Animacje: nazwa -> (numery klatek shimeN.png, ile ticków trwa jedna klatka).
// Numery zgodne ze standardową paczką Shimeji (sprawdzone na paczce z Miku).
struct AnimDef {
    const char *name;
    std::vector<int> nums;
    int ticks;
};

const std::vector<AnimDef> &shimejiAnims()
{
    static const std::vector<AnimDef> defs = {
        {"stand", {1}, 1},
        {"walk", {1, 2, 1, 3}, 6},
        {"fall", {4}, 1},
        {"drag", {5}, 1},
        {"sit", {11}, 1},
        {"sit_look", {26, 15, 27, 16, 28, 17, 29, 11}, 10},   // siedzi i rozgląda się
        {"sit_legs", {31, 32, 31, 33}, 8},                    // siedzi i macha nogami
        {"lie", {21, 20}, 35},                                // leży i śpi (powolny oddech)
        {"surprised", {37}, 1},                               // "!" po kliknięciu
        {"grumpy", {39, 40, 41, 40}, 6},                      // marudzi po upuszczeniu
        {"land", {18}, 1},                                    // "plask" przy lądowaniu
        {"leek", {42, 43, 44, 43}, 5},                        // macha porem
        // Brak actions.xml w paczce - tempo dobrane na oko.
        {"sit_happy", {12, 13, 12, 14}, 8},                   // siedzi wesoło
        {"sit_think", {30, 30, 11}, 25},                      // siedzi zamyślona (czasem zwykłe siedzenie)
        {"shy", {6, 7, 6, 8}, 10},                            // zawstydzona
        {"ponder", {38, 1}, 30},                              // zamyślona na stojąco
        {"cheer", {22, 1, 22, 1}, 6},                         // radość, ręce w górze
        {"dance", {22, 12, 13, 14, 22, 19}, 6},               // taniec
    };
    return defs;
}

bool isRequired(const QString &name)
{
    return name == QLatin1String("stand") || name == QLatin1String("walk")
        || name == QLatin1String("fall") || name == QLatin1String("drag");
}

}  // namespace

QStringList shimejiAnimNames()
{
    QStringList names;
    for (const AnimDef &def : shimejiAnims())
        names << QString::fromLatin1(def.name);
    return names;
}

QList<int> shimejiAnimFrames(const QString &name)
{
    for (const AnimDef &def : shimejiAnims()) {
        if (name == QLatin1String(def.name))
            return QList<int>(def.nums.begin(), def.nums.end());
    }
    return {};
}

bool loadShimeji(const QString &folder, double scale, Sprites *out, QString *missing)
{
    Sprites sp;
    QHash<QString, bool> have;
    for (const AnimDef &def : shimejiAnims()) {
        Anim a;
        a.ticks = def.ticks;
        bool ok = true;
        for (int n : def.nums) {
            const QString path = QStringLiteral("%1/shime%2.png").arg(folder).arg(n);
            QPixmap pm(path);
            if (pm.isNull()) {
                if (isRequired(QString::fromLatin1(def.name))) {
                    *missing = path;
                    return false;
                }
                ok = false;
                break;
            }
            if (scale != 1.0)
                pm = pm.scaled(pm.size() * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            a.frames << pm;
        }
        if (ok)
            sp.anims.insert(QString::fromLatin1(def.name), a);
    }
    // Brakujące klatki (inne paczki) zastępuje stojąca postać.
    for (const AnimDef &def : shimejiAnims()) {
        const QString name = QString::fromLatin1(def.name);
        if (!sp.anims.contains(name))
            sp.anims.insert(name, sp.anims.value(QStringLiteral("stand")));
    }
    sp.facesLeft = true;
    sp.buildMirrored();
    *out = sp;
    return true;
}

Sprites placeholderSprites()
{
    auto frame = [](int legShift = 0, bool armsUp = false, int squash = 0) {
        QPixmap pm(96, 96);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor("#333"));
        p.setBrush(QColor("#39c5bb"));
        p.drawEllipse(QRectF(20, 8 + squash, 56, 52 - squash));      // włosy/głowa
        p.setBrush(QColor("#ffe0cc"));
        p.drawEllipse(QRectF(26, 18 + squash, 44, 38 - squash));     // twarz
        p.setBrush(QColor("#222"));
        p.drawEllipse(QRectF(33, 34, 6, 8));
        p.drawEllipse(QRectF(51, 34, 6, 8));
        p.setBrush(QColor("#555"));
        p.drawRect(QRectF(36, 58, 24, 22));                          // tułów
        p.drawLine(40 - legShift, 80, 40 - legShift, 94);
        p.drawLine(56 + legShift, 80, 56 + legShift, 94);
        if (armsUp) {
            p.drawLine(36, 62, 28, 46);
            p.drawLine(60, 62, 68, 46);
        }
        p.end();
        return pm;
    };

    Sprites sp;
    const Anim stand{{frame()}, 1, {}};
    const Anim up{{frame(0, true)}, 1, {}};
    for (const char *n : {"stand", "sit", "sit_look", "sit_legs", "sit_think", "ponder"})
        sp.anims.insert(QString::fromLatin1(n), stand);
    // Wesołe/zawstydzone: ręce w górze na zmianę ze stojącą (widać, że coś się dzieje).
    const Anim lively{{frame(0, true), frame()}, 8, {}};
    for (const char *n : {"sit_happy", "shy", "cheer", "dance"})
        sp.anims.insert(QString::fromLatin1(n), lively);
    sp.anims.insert(QStringLiteral("lie"), Anim{{frame(0, false, 6), frame(0, false, 5)}, 35, {}});
    sp.anims.insert(QStringLiteral("walk"), Anim{{frame(), frame(3), frame(), frame(-3)}, 6, {}});
    for (const char *n : {"fall", "surprised", "leek", "grumpy", "land"})
        sp.anims.insert(QString::fromLatin1(n), up);
    sp.anims.insert(QStringLiteral("drag"), Anim{{frame(0, true, 2)}, 1, {}});
    sp.facesLeft = true;
    sp.buildMirrored();
    return sp;
}

Sprites loadSprites()
{
    const QString folder = findShimejiDir(cfg::spritesDir());
    if (!folder.isEmpty()) {
        Sprites sp;
        QString missing;
        std::printf("desktop-pet: sprite'y z %s\n", qPrintable(folder));
        std::fflush(stdout);
        if (loadShimeji(folder, cfg::spriteScale(), &sp, &missing))
            return sp;
        std::fprintf(stderr, "desktop-pet: brak klatki %s, używam postaci zastępczej\n",
                     qPrintable(missing));
    } else {
        std::printf("desktop-pet: brak shime1.png w %s, używam postaci zastępczej\n",
                    qPrintable(cfg::spritesDir()));
        std::fflush(stdout);
    }
    return placeholderSprites();
}
