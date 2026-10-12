// Parser tekstowego VDF/ACF Valve (KeyValues) - tylko odczyt.
#pragma once

#include <QString>
#include <vector>

struct VdfNode {
    bool dict = false;                // sekcja { … } albo zwykły napis
    QString text;                     // wartość napisu
    std::vector<QString> keys;        // klucze sekcji w kolejności wstawienia
    std::vector<VdfNode> vals;

    // Ostatni element o kluczu key (bez rozróżniania wielkości liter) albo nullptr.
    const VdfNode *find(const QString &key) const;
    // Jak przypisanie do dicta: istniejący klucz jest nadpisywany w miejscu.
    VdfNode &set(const QString &key, VdfNode value);
};

// Błędy składni nie przerywają odczytu (nadmiarowe "}" jest ignorowane).
VdfNode parseVdf(const QString &text);
VdfNode parseVdfFile(const QString &path);   // brak pliku = pusta sekcja
