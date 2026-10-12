# Plan nowych animacji desktop-pet (Miku, sprite'y Shimeji)

Paczka: ~/.config/desktop-pet/sprites/, 46 klatek shime1..46.png, BRAK actions.xml/behaviors.xml (tylko PNG),
więc sekwencje i tempo poniżej to propozycje wywnioskowane z wyglądu klatek (do sprawdzenia wizualnie po wdrożeniu).
Rozmiary: większość 128x128; 12,14,22,30-41 = 125x125; 9,10 = 170x128; 23-25 = 125x166 (wysokie, postać pionowo);
42-46 = 166x125 (szersze, por). Okno ma stały pad = najszersza klatka (170) - nowe klatki mieszczą się bez zmian.
Tempo: ticks na klatkę, 25 ticków = 1 s. Anim = {nazwa, {klatki}, ticks} w sprites.cpp (defs, ok. linii 51-62);
aktywności trybów w pet.cpp modes() (nazwa, waga, min, max ticków); reakcje przez react({{"nazwa", ticki}}).
Uwaga: loadShimeji wymaga klatek z defs; brakujące wpisy trzeba też dodać do fallbacku (sprites.cpp ok. linii 142-148).

## Opis nieużywanych klatek
- 6: smutna/zawstydzona, siedzi, patrzy na wprost, czerwone policzki (tej samej rodziny co 5 "drag"; "Pinched"/zakłopotanie)
- 7: jw., rękawy rozłożone szerzej, lekko zwrócona
- 8: zawstydzona, ciało bokiem/trzy czwarte, patrzy w bok
- 9, 10 (170x128): w powietrzu, włosy rozwiane, głowa przechylona; 10 mocniej zwinięta - skok/salto/obrót (Jumping lub zwis u sufitu)
- 12, 13, 14: siedzi, uśmiech z otwartymi ustami, wesoła; 12 patrzy na wprost, 13 lekko w bok, 14 w bok z uniesionym kolanem
- 19: w skoku, z wyciągniętą nogą i ręką, włosy do góry (Jump / kopnięcie)
- 22: radosna, ręce uniesione nad głową, włosy rozwiane (Cheer / zwis u sufitu)
- 23, 24, 25 (125x166): postać pionowo przy ścianie, głowa w bok, włosy opadają w dół, 3 lekko różne pozy - ClimbWall / wisi na krawędzi ekranu
- 30: siedzi zamyślona/senna, kolana pod brodą (spokojny wariant 11)
- 34, 35, 36: widok z tyłu/z boku, głowa ucięta u góry (klatka rysowana do łączenia z inną), nogi chodzą 35/36 - prawdopodobnie Creep/ czołganie pod sufitem lub chodzenie po suficie; DO WERYFIKACJI (wyglądają na niekompletne)
- 38: stoi, ręka przy buzi, lekko zdziwiona/zamyślona
- 45, 46 (166x125): por: 45 trzyma por w uniesionej ręce, obok mała głowa (efekt "rzutu"/ ThrowIE); 46 podobnie, por wyżej - ciąg do 42-44 (dodatkowa animacja "rzut/podanie pora")

## Propozycje
Legenda trudności: Ł = tylko tabela klatek, S = nowy stan ruchu / logika, T = trudna (nowa fizyka).

### A. Nowe aktywności w istniejących trybach (Ł)
1. Siedzi wesoło ("sit_happy"): 12,13,12,14, ticks 8. Tryb "sit" i "auto", waga 2, 100-250 ticków. Ł
2. Siedzi zamyślona ("sit_think"): 30 (+ 11 co jakiś czas, np. {30,30,11}, ticks 25). "sit"/"auto", waga 1. Ł
3. Zawstydzona ("shy"): 6,7,6,8, ticks 10. Aktywność w "stand"/"auto" (waga 1) i jako reakcja na ponowny klik. Ł
4. Zamyślona na stojąco ("ponder"): 38, 1, ticks 30. "stand" i "auto". Ł
5. Radość ("cheer"): 22,1,22,1 ticks 6 (ręce w górze). Aktywność w "auto" + reakcja po zmianie utworu MPRIS (dymek muzyczny). Ł (reakcja MPRIS: S)
6. Wesoła chwila na stojąco ("stand_happy"): 38,37 nie - 37 to "!" ; lepiej 22 w parze z 1. Ł

### B. Nowe tryby w menu "Co ma robić?"
7. Tryb "Tańczy" ("dance"): sekwencja 22,12,13,14,22,19 (ticks 6) + stand; wagi {dance 3, stand 1}. Ł (efekt tańca jest niski - klatki nie są dedykowane; skromny efekt)
8. Tryb "Wstydliwa" ("shy"): 6,7,8 + sit. Ł
9. Tryb "Wesoła" ("happy"): miks 12,13,14,22,38,sit_legs z małymi wagami. Ł
10. Tryb "Skacze" ("jump"): stand -> 19 (zamach, 5 ticków) -> 9 -> 10 -> 9 w powietrzu z vy=-10 (nowy stan Jump, grawitacja już jest) -> land. S

### C. Reakcje
11. Klik na pet (obecnie "surprised" 20 ticków): losowo surprised / shy (6,7) / cheer (22). Ł
12. Kółko myszy: obecnie "surprised"; dodać cykl 38 -> 12 -> 22 przy szybkim kręceniu (licznik w wheelEvent). Ł
13. Po upadku z małej wysokości (vy<=12): zamiast nic - 10 ticków "land" 18 lub "shy". Ł
14. Po rzuceniu (puszczeniu w górę, vy<0): lot 9/10 zamiast "fall" 4 (obrót w powietrzu: 9,10,9,10 ticks 5; sprites.cpp: dodać anim "fly", w step() użyć jej gdy m_vy < 0 lub przez pierwsze ticki). S (niewielka)
15. Dymek/czat Claude: podczas odpowiedzi "ponder" 38; po odpowiedzi "cheer" 22. S
16. Pora dnia/bezczynność: po N minutach bez interakcji zasypia ("lie") - już istnieje; dodać ziewanie nie ma klatek - pominąć.

### D. Ruchy z fizyką
17. Wspinanie po bocznej krawędzi ekranu ("climb"): 23,24,25,24, ticks 8, obrót/ustawienie przy lewej/prawej krawędzi workarea, ruch pionowy 1-2 px/tick w górę, potem spadek. Klatki 125x166 - wysokie, okno ma wysokość 128 (setFixedSize body.height) - trzeba podnieść wysokość okna do 166 i dopasować m_pos/floorY oraz maskę. T
18. Wisienie u sufitu ("ceiling"): klatki 9/10 odwrócone o 180 st. (lub 22), zwis na górnej krawędzi workarea, chodzenie po suficie z 34-36 (do weryfikacji). Nowy stan State::Ceiling, brak sufitu w workarea (panel u góry), trzeba odwracanie QTransform. T
19. Skok ("jump"): patrz 10 - stan State::Jump (vy ujemne). Dla krawędzi: "skacze na ścianę" 19 -> 23. T/S
20. Czołganie ("creep") 34-36: tylko jeśli klatki okażą się kompletne - zrobić zrzut po złożeniu (offscreen) przed wdrożeniem. T (niepewne)
21. Rzut porem ("throw_leek"): 45,46,44 (ticks 6), dodatkowy efekt: por jako osobny sprite (nie ma - trzeba rysować w paintEvent). S

## Rekomendowana kolejność
1. Pakiet Ł: punkty 1,2,3,4,5 + reakcje 11,13 (jeden commit, tylko defs + modes). 
2. Tryb "Wesoła" i "Tańczy" (7,9).
3. Lot po rzucie 14 oraz skok 10.
4. Wspinanie 17 (dopiero po decyzji o podniesieniu okna do 166 px).
5. Sufit 18 i czołganie 20 - tylko jeśli user chce; wymagają weryfikacji klatek 34-36.

## Obawy
- Brak actions.xml: wszystkie sekwencje, tempo i znaczenie klatek to domysły z wyglądu.
- 23-25 i 9-10 mają inne wymiary niż 128x128; kotwica przy dolnej krawędzi (floorY) i facesLeft muszą być sprawdzone, inaczej postać "pływa" nad podłogą.
- 34-36 wyglądają na ucięte u góry; mogą być klatkami pomocniczymi, nie samodzielnymi.
- Nie sprawdzono na żywym pulpicie (zakaz); jakość animacji tylko z klatek statycznych.
- Wszystkie nowe anim muszą być też w fallbacku (postać zastępcza), inaczej tryb bez paczki sprite'ów się wywróci lub użyje "stand".
