#pragma once

#include "model/Topology.hpp"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QTextEdit;

namespace bgptester
{

class RouterDialog final : public QDialog
{
    Q_OBJECT

public:
    RouterDialog(const RouterConfig& router, const QStringList& otherRouterIds, QWidget* parent = nullptr);
    RouterConfig router() const;

public slots:
    void accept() override;

private:
    RouterConfig original_;
    QStringList otherRouterIds_;
    QLineEdit* idEdit_ = nullptr;
    QLineEdit* routerIdEdit_ = nullptr;
    QLineEdit* asnEdit_ = nullptr;
    QLineEdit* clusterIdEdit_ = nullptr;
    QTextEdit* prefixesEdit_ = nullptr;
    QComboBox* pluginCombo_ = nullptr;
    QLabel* pluginDescriptionLabel_ = nullptr;
    QTextEdit* pluginSettingsEdit_ = nullptr;
};

class LinkDialog final : public QDialog
{
    Q_OBJECT

public:
    LinkDialog(const LinkConfig& link, bool externalSession, QWidget* parent = nullptr);
    LinkConfig link() const;

private:
    LinkConfig original_;
    QCheckBox* enabledCheck_ = nullptr;
    QSpinBox* delaySpin_ = nullptr;
    QComboBox* relationshipCombo_ = nullptr;
    QCheckBox* rrFromACheck_ = nullptr;
    QCheckBox* rrFromBCheck_ = nullptr;
    QSpinBox* mraiFromASpin_ = nullptr;
    QSpinBox* mraiFromBSpin_ = nullptr;
};

class TopologyBatchEditDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class DelayMode
    {
        Unchanged,
        Fixed,
        RandomRange,
    };

    enum class RouterScope
    {
        All,
        Selection,
    };

    enum class MraiMode
    {
        Unchanged,
        Fixed,
        RandomRange,
    };

    explicit TopologyBatchEditDialog(const QVector<LinkConfig>& links, const QVector<RouterConfig>& routers, RouterScope routerScope,
                                     QWidget* parent = nullptr);

    DelayMode delayMode() const;
    int fixedDelayMs() const;
    int minimumDelayMs() const;
    int maximumDelayMs() const;
    MraiMode mraiMode() const;
    int fixedMraiMs() const;
    int minimumMraiMs() const;
    int maximumMraiMs() const;
    QString routerPluginId() const;
    QJsonObject routerPluginDefaultSettings() const;

public slots:
    void accept() override;

private:
    QRadioButton* unchangedDelayRadio_ = nullptr;
    QRadioButton* fixedDelayRadio_ = nullptr;
    QSpinBox* fixedDelaySpin_ = nullptr;
    QRadioButton* randomDelayRadio_ = nullptr;
    QSpinBox* minimumDelaySpin_ = nullptr;
    QSpinBox* maximumDelaySpin_ = nullptr;
    QComboBox* routerPluginCombo_ = nullptr;
    QLabel* routerPluginDescriptionLabel_ = nullptr;
    QRadioButton* unchangedMraiRadio_ = nullptr;
    QRadioButton* fixedMraiRadio_ = nullptr;
    QSpinBox* fixedMraiSpin_ = nullptr;
    QRadioButton* randomMraiRadio_ = nullptr;
    QSpinBox* minimumMraiSpin_ = nullptr;
    QSpinBox* maximumMraiSpin_ = nullptr;
};

class SimulationSettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit SimulationSettingsDialog(const SimulationSettings& settings, QWidget* parent = nullptr);
    SimulationSettings settings() const;

public slots:
    void accept() override;

private:
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* logDirectoryEdit_ = nullptr;
    QSpinBox* workerThreadsSpin_ = nullptr;
    QSpinBox* quietSpin_ = nullptr;
    QCheckBox* withdrawalIgnoresMraiCheck_ = nullptr;
};

} // namespace bgptester
