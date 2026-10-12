// Sprite'y: paczka Shimeji z ~/.config/desktop-pet/sprites albo postać zastępcza.
#pragma once

#include <QHash>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QStringList>

// Jedna animacja: klatki i liczba ticków trwania jednej klatki.
struct Anim {
    QList<QPixmap> frames;
    int ticks = 1;
    QList<QPixmap> mirrored = {};   // lustrzane odbicia klatek, liczone raz przy ładowaniu
};

struct Sprites {
    QHash<QString, Anim> anims;
    bool facesLeft = true;   // oryginalne klatki patrzą w lewo
    const Anim &anim(const QString &name) const;
    // Uzupełnia Anim::mirrored dla wszystkich animacji (raz po załadowaniu).
    void buildMirrored();
};

// Folder, w którym leży shime1.png (płytszy i wcześniejszy alfabetycznie wygrywa) albo "".
QString findShimejiDir(const QString &root);

// Nazwy animacji paczki Shimeji i numery ich klatek (do testów i kontroli spójności).
QStringList shimejiAnimNames();
QList<int> shimejiAnimFrames(const QString &name);

// Mapuje shimeN.png na animacje; false (i missing = ścieżka brakującej klatki), gdy
// brakuje klatek obowiązkowych (stand, walk, fall, drag).
bool loadShimeji(const QString &folder, double scale, Sprites *out, QString *missing);

Sprites placeholderSprites();   // prosta rysowana postać (patrzy w lewo)
Sprites loadSprites();          // wybiera Shimeji albo postać zastępczą, loguje wybór

// Pozycja y klatki w oknie: klatka przylega do dołu okna (stopy na podłodze), także gdy jest
// niższa niż okno (np. 125 px w oknie 128 px). Wyższa niż okno jest przycinana od góry.
inline int frameY(int windowHeight, int frameHeight)
{
    return windowHeight > frameHeight ? windowHeight - frameHeight : 0;
}
