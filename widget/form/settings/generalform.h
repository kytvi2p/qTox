/*
    Copyright (C) 2014 by Project Tox <https://tox.im>

    This file is part of qTox, a Qt-based graphical interface for Tox.

    This program is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

    See the COPYING file for more details.
*/

#ifndef GENERALFORM_H
#define GENERALFORM_H

#include "genericsettings.h"
#include <QComboBox>
#include <QCheckBox>

namespace Ui {
class GeneralSettings;
}

class GeneralForm : public GenericForm
{
    Q_OBJECT
public:
    GeneralForm();
    ~GeneralForm();

private slots:
    void onEnableIPv6Updated();
    void onUseTranslationUpdated();
    void onMakeToxPortableUpdated();
    void onSmileyBrowserIndexChanged(int index);
    void onUDPUpdated();
    void onProxyAddrEdited();
    void onProxyPortEdited(int port);
    void onUseProxyUpdated();

private:
    Ui::GeneralSettings *bodyUI;
};

#endif
