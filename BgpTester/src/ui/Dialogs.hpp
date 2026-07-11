#pragma once

#include "model/Topology.hpp"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QLineEdit;
class QSpinBox;
class QTextEdit;

namespace bgptester {

class RouterDialog final : public QDialog {
  Q_OBJECT

public:
  RouterDialog(const RouterConfig &router, const QStringList &otherRouterIds,
               QWidget *parent = nullptr);
  [[nodiscard]] RouterConfig router() const;

public slots:
  void accept() override;

private:
  RouterConfig original_;
  QStringList otherRouterIds_;
  QLineEdit *idEdit_ = nullptr;
  QLineEdit *routerIdEdit_ = nullptr;
  QLineEdit *asnEdit_ = nullptr;
  QLineEdit *clusterIdEdit_ = nullptr;
  QTextEdit *prefixesEdit_ = nullptr;
};

class LinkDialog final : public QDialog {
  Q_OBJECT

public:
  explicit LinkDialog(const LinkConfig &link, QWidget *parent = nullptr);
  [[nodiscard]] LinkConfig link() const;

private:
  LinkConfig original_;
  QCheckBox *enabledCheck_ = nullptr;
  QSpinBox *delaySpin_ = nullptr;
  QCheckBox *rrFromACheck_ = nullptr;
  QCheckBox *rrFromBCheck_ = nullptr;
  QSpinBox *mraiFromASpin_ = nullptr;
  QSpinBox *mraiFromBSpin_ = nullptr;
};

class SimulationSettingsDialog final : public QDialog {
  Q_OBJECT

public:
  explicit SimulationSettingsDialog(const SimulationSettings &settings,
                                    QWidget *parent = nullptr);
  [[nodiscard]] SimulationSettings settings() const;

public slots:
  void accept() override;

private:
  QLineEdit *nameEdit_ = nullptr;
  QLineEdit *logDirectoryEdit_ = nullptr;
  QSpinBox *quietSpin_ = nullptr;
};

} // namespace bgptester

