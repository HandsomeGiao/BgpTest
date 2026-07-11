#include "ui/Dialogs.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>

#include <limits>

namespace bgptester {

RouterDialog::RouterDialog(const RouterConfig &router,
                           const QStringList &otherRouterIds, QWidget *parent)
    : QDialog(parent), original_(router), otherRouterIds_(otherRouterIds) {
  setWindowTitle(QStringLiteral("路由器属性"));
  setMinimumWidth(430);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  idEdit_ = new QLineEdit(router.id, this);
  routerIdEdit_ = new QLineEdit(router.routerId, this);
  asnEdit_ = new QLineEdit(QString::number(router.asn), this);
  asnEdit_->setValidator(new QRegularExpressionValidator(
      QRegularExpression(QStringLiteral("[0-9]{1,10}")), asnEdit_));
  clusterIdEdit_ = new QLineEdit(router.clusterId, this);
  clusterIdEdit_->setPlaceholderText(QStringLiteral("默认与 Router ID 相同"));
  prefixesEdit_ = new QTextEdit(this);
  prefixesEdit_->setAcceptRichText(false);
  prefixesEdit_->setPlainText(router.originatedPrefixes.join(u'\n'));
  prefixesEdit_->setMinimumHeight(110);
  pluginCombo_ = new QComboBox(this);
  for (const auto &plugin : RouterPluginRegistry::instance().plugins()) {
    pluginCombo_->addItem(
        QStringLiteral("%1  (%2)")
            .arg(plugin.metadata.displayName, plugin.metadata.id),
        plugin.metadata.id);
  }
  auto pluginIndex = pluginCombo_->findData(router.pluginId);
  if (pluginIndex < 0) {
    pluginCombo_->addItem(
        QStringLiteral("未加载：%1").arg(router.pluginId), router.pluginId);
    pluginIndex = pluginCombo_->count() - 1;
  }
  pluginCombo_->setCurrentIndex(pluginIndex);
  pluginDescriptionLabel_ = new QLabel(this);
  pluginDescriptionLabel_->setWordWrap(true);
  pluginDescriptionLabel_->setTextInteractionFlags(
      Qt::TextSelectableByMouse);
  pluginSettingsEdit_ = new QTextEdit(this);
  pluginSettingsEdit_->setAcceptRichText(false);
  pluginSettingsEdit_->setMinimumHeight(90);
  pluginSettingsEdit_->setPlainText(QString::fromUtf8(
      QJsonDocument(router.pluginSettings).toJson(QJsonDocument::Indented)));

  const auto updateDescription = [this] {
    const auto id = pluginCombo_->currentData().toString();
    const auto metadata = RouterPluginRegistry::instance().metadata(id);
    pluginDescriptionLabel_->setText(
        metadata ? metadata->description
                 : QStringLiteral("该插件当前未加载；拓扑可以保存，但无法启动仿真。"));
  };
  updateDescription();
  connect(pluginCombo_, &QComboBox::activated, this,
          [this, updateDescription](int) {
            updateDescription();
            const auto metadata = RouterPluginRegistry::instance().metadata(
                pluginCombo_->currentData().toString());
            if (metadata) {
              pluginSettingsEdit_->setPlainText(QString::fromUtf8(
                  QJsonDocument(metadata->defaultSettings)
                      .toJson(QJsonDocument::Indented)));
            }
          });
  form->addRow(QStringLiteral("节点 ID"), idEdit_);
  form->addRow(QStringLiteral("BGP Router ID"), routerIdEdit_);
  form->addRow(QStringLiteral("ASN"), asnEdit_);
  form->addRow(QStringLiteral("Cluster ID"), clusterIdEdit_);
  form->addRow(QStringLiteral("本地起源前缀（每行一个）"), prefixesEdit_);
  form->addRow(QStringLiteral("路由器插件"), pluginCombo_);
  form->addRow(QStringLiteral("插件说明"), pluginDescriptionLabel_);
  form->addRow(QStringLiteral("插件配置（JSON 对象）"), pluginSettingsEdit_);
  layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &RouterDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

RouterConfig RouterDialog::router() const {
  auto result = original_;
  result.id = idEdit_->text().trimmed();
  result.routerId = routerIdEdit_->text().trimmed();
  result.asn = asnEdit_->text().toUInt();
  result.clusterId = clusterIdEdit_->text().trimmed();
  if (result.clusterId.isEmpty()) {
    result.clusterId = result.routerId;
  }
  result.originatedPrefixes.clear();
  for (const auto &line : prefixesEdit_->toPlainText().split(u'\n')) {
    const auto prefix = line.trimmed();
    if (!prefix.isEmpty() && !result.originatedPrefixes.contains(prefix)) {
      result.originatedPrefixes.append(prefix);
    }
  }
  result.pluginId = pluginCombo_->currentData().toString().trimmed();
  const auto settingsDocument = QJsonDocument::fromJson(
      pluginSettingsEdit_->toPlainText().trimmed().toUtf8());
  if (settingsDocument.isObject()) {
    result.pluginSettings = settingsDocument.object();
  }
  return result;
}

void RouterDialog::accept() {
  const auto result = router();
  bool asnOk = false;
  const auto asn = asnEdit_->text().toULongLong(&asnOk);
  if (result.id.isEmpty() || otherRouterIds_.contains(result.id)) {
    QMessageBox::warning(this, QStringLiteral("输入无效"),
                         result.id.isEmpty()
                             ? QStringLiteral("节点 ID 不能为空。")
                             : QStringLiteral("节点 ID 已存在：%1").arg(result.id));
    return;
  }
  if (!asnOk || asn == 0 ||
      asn > std::numeric_limits<quint32>::max()) {
    QMessageBox::warning(this, QStringLiteral("输入无效"),
                         QStringLiteral("ASN 必须在 1 到 4294967295 之间。"));
    return;
  }
  QJsonParseError settingsError;
  const auto settingsDocument = QJsonDocument::fromJson(
      pluginSettingsEdit_->toPlainText().trimmed().toUtf8(), &settingsError);
  if (settingsError.error != QJsonParseError::NoError ||
      !settingsDocument.isObject()) {
    QMessageBox::warning(
        this, QStringLiteral("输入无效"),
        settingsError.error == QJsonParseError::NoError
            ? QStringLiteral("插件配置必须是 JSON 对象。")
            : QStringLiteral("插件配置 JSON 无效（偏移 %1）：%2")
                  .arg(settingsError.offset)
                  .arg(settingsError.errorString()));
    return;
  }
  Topology topology;
  topology.routers.insert(result.id, result);
  const auto problems = topology.validate();
  if (!problems.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("输入无效"), problems.join(u'\n'));
    return;
  }
  QDialog::accept();
}

LinkDialog::LinkDialog(const LinkConfig &link, QWidget *parent)
    : QDialog(parent), original_(link) {
  setWindowTitle(QStringLiteral("链路属性：%1 ↔ %2").arg(link.a, link.b));
  setMinimumWidth(450);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  enabledCheck_ = new QCheckBox(QStringLiteral("初始启用"), this);
  enabledCheck_->setChecked(link.enabled);
  delaySpin_ = new QSpinBox(this);
  delaySpin_->setRange(0, 24 * 60 * 60 * 1000);
  delaySpin_->setSuffix(QStringLiteral(" ms"));
  delaySpin_->setValue(link.delayMs);
  rrFromACheck_ = new QCheckBox(
      QStringLiteral("%1 将 %2 视为 RR Client").arg(link.a, link.b), this);
  rrFromACheck_->setChecked(link.rrClientFromA);
  rrFromBCheck_ = new QCheckBox(
      QStringLiteral("%1 将 %2 视为 RR Client").arg(link.b, link.a), this);
  rrFromBCheck_->setChecked(link.rrClientFromB);
  mraiFromASpin_ = new QSpinBox(this);
  mraiFromASpin_->setRange(0, 24 * 60 * 60 * 1000);
  mraiFromASpin_->setSuffix(QStringLiteral(" ms"));
  mraiFromASpin_->setValue(link.mraiMsFromA);
  mraiFromBSpin_ = new QSpinBox(this);
  mraiFromBSpin_->setRange(0, 24 * 60 * 60 * 1000);
  mraiFromBSpin_->setSuffix(QStringLiteral(" ms"));
  mraiFromBSpin_->setValue(link.mraiMsFromB);
  form->addRow(QStringLiteral("状态"), enabledCheck_);
  form->addRow(QStringLiteral("链路延迟"), delaySpin_);
  form->addRow(QStringLiteral("%1 → %2 MRAI").arg(link.a, link.b),
               mraiFromASpin_);
  form->addRow(QStringLiteral("%1 → %2 MRAI").arg(link.b, link.a),
               mraiFromBSpin_);
  form->addRow(QStringLiteral("路由反射"), rrFromACheck_);
  form->addRow(QString{}, rrFromBCheck_);
  layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

LinkConfig LinkDialog::link() const {
  auto result = original_;
  result.enabled = enabledCheck_->isChecked();
  result.delayMs = delaySpin_->value();
  result.rrClientFromA = rrFromACheck_->isChecked();
  result.rrClientFromB = rrFromBCheck_->isChecked();
  result.mraiMsFromA = mraiFromASpin_->value();
  result.mraiMsFromB = mraiFromBSpin_->value();
  return result;
}

SimulationSettingsDialog::SimulationSettingsDialog(
    const SimulationSettings &settings, QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("仿真设置"));
  setMinimumWidth(430);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  nameEdit_ = new QLineEdit(settings.name, this);
  logDirectoryEdit_ = new QLineEdit(settings.logDirectory, this);
  quietSpin_ = new QSpinBox(this);
  quietSpin_->setRange(0, 600000);
  quietSpin_->setSuffix(QStringLiteral(" ms"));
  quietSpin_->setValue(settings.convergenceQuietMs);
  form->addRow(QStringLiteral("实验名称"), nameEdit_);
  form->addRow(QStringLiteral("日志目录"), logDirectoryEdit_);
  form->addRow(QStringLiteral("收敛静默窗口"), quietSpin_);
  layout->addLayout(form);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &SimulationSettingsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

SimulationSettings SimulationSettingsDialog::settings() const {
  return SimulationSettings{.name = nameEdit_->text().trimmed(),
                            .logDirectory =
                                logDirectoryEdit_->text().trimmed(),
                            .convergenceQuietMs = quietSpin_->value()};
}

void SimulationSettingsDialog::accept() {
  if (nameEdit_->text().trimmed().isEmpty() ||
      logDirectoryEdit_->text().trimmed().isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("输入无效"),
                         QStringLiteral("实验名称和日志目录不能为空。"));
    return;
  }
  QDialog::accept();
}

} // namespace bgptester
