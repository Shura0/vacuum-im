#ifndef QTCOMPAT_H
#define QTCOMPAT_H

#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>

// Qt 6 removed the implicit QList/QVector -> QSet conversion helpers
// (QList::toSet(), QSet::toList()). This helper provides an equivalent
// for the QList::toSet() case while evaluating the source container once.
template <class Container>
inline QSet<typename Container::value_type> qToSet(const Container &AContainer)
{
	using T = typename Container::value_type;
	return QSet<T>(AContainer.cbegin(), AContainer.cend());
}

// Qt 6 removed the <QtAlgorithms> comparison functors qLess/qGreater.
// Unlike std::less/std::greater they only require operator<, so they are
// useful for types that do not define operator>.
template <typename T>
struct qLess
{
	bool operator()(const T &t1, const T &t2) const { return t1 < t2; }
};

template <typename T>
struct qGreater
{
	bool operator()(const T &t1, const T &t2) const { return t2 < t1; }
};

#endif // QTCOMPAT_H
