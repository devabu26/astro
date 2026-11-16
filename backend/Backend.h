#pragma once

#include <QObject>
#include <QString>

class Backend : public QObject
{
    Q_OBJECT

public:
    explicit Backend(QObject *parent = nullptr);

    Q_INVOKABLE QString status() const;
};