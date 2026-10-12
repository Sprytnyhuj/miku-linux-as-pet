#include "vdf.h"

#include <QFile>

namespace {

struct Token {
    bool isString;   // false = "{" albo "}"
    QString text;
};

// Tokeny formatu KeyValues.
std::vector<Token> tokenize(const QString &text)
{
    std::vector<Token> out;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = text[i];
        if (c.isSpace()) {
            ++i;
        } else if (text.mid(i, 2) == QLatin1String("//")) {        // komentarz do końca linii
            i = text.indexOf(QLatin1Char('\n'), i);
            if (i < 0)
                i = n;
        } else if (c == QLatin1Char('{') || c == QLatin1Char('}')) {
            out.push_back({false, QString(c)});
            ++i;
        } else if (c == QLatin1Char('"')) {
            QString s;
            ++i;
            while (i < n && text[i] != QLatin1Char('"')) {
                if (text[i] == QLatin1Char('\\') && i + 1 < n) {
                    const QChar e = text[i + 1];
                    if (e == QLatin1Char('n'))
                        s += QLatin1Char('\n');
                    else if (e == QLatin1Char('t'))
                        s += QLatin1Char('\t');
                    else if (e == QLatin1Char('\\') || e == QLatin1Char('"'))
                        s += e;
                    else
                        s += QString(QLatin1Char('\\')) + e;
                    i += 2;
                } else {
                    s += text[i];
                    ++i;
                }
            }
            out.push_back({true, s});
            ++i;                                                  // zamykający cudzysłów
        } else {                                                  // wartość bez cudzysłowów
            qsizetype j = i;
            while (j < n && !text[j].isSpace() && text[j] != QLatin1Char('{')
                   && text[j] != QLatin1Char('}') && text[j] != QLatin1Char('"'))
                ++j;
            const QString word = text.mid(i, j - i);
            if (!(word.startsWith(QLatin1Char('[')) && word.endsWith(QLatin1Char(']'))))   // warunki typu [$WIN32]
                out.push_back({true, word});
            i = j;
        }
    }
    return out;
}

}  // namespace

const VdfNode *VdfNode::find(const QString &key) const
{
    for (size_t i = keys.size(); i-- > 0;) {
        if (keys[i].compare(key, Qt::CaseInsensitive) == 0)
            return &vals[i];
    }
    return nullptr;
}

VdfNode &VdfNode::set(const QString &key, VdfNode value)
{
    dict = true;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            vals[i] = std::move(value);
            return vals[i];
        }
    }
    keys.push_back(key);
    vals.push_back(std::move(value));
    return vals.back();
}

VdfNode parseVdf(const QString &text)
{
    VdfNode root;
    root.dict = true;
    std::vector<VdfNode *> stack{&root};
    QString key;
    bool haveKey = false;
    for (const Token &t : tokenize(text)) {
        if (t.isString) {
            if (!haveKey) {
                key = t.text;
                haveKey = true;
            } else {
                VdfNode v;
                v.text = t.text;
                stack.back()->set(key, std::move(v));
                haveKey = false;
            }
        } else if (t.text == QLatin1String("{")) {
            VdfNode child;
            child.dict = true;
            VdfNode &slot = stack.back()->set(haveKey ? key : QString(), std::move(child));
            stack.push_back(&slot);
            haveKey = false;
        } else {                                                  // "}" - nadmiarowy jest ignorowany
            if (stack.size() > 1)
                stack.pop_back();
            haveKey = false;
        }
    }
    return root;
}

VdfNode parseVdfFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        VdfNode empty;
        empty.dict = true;
        return empty;
    }
    return parseVdf(QString::fromUtf8(f.readAll()));
}
