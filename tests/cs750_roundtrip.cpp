#include "../src/codeplugdefinition.h"
#include "../src/repeaterstore.h"
#include "../src/ziparchive.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QDebug>
#include <algorithm>
#include <stdexcept>
static void check(bool ok,const QString &why){if(!ok)throw std::runtime_error(why.toStdString());}
int main(int argc,char **argv){QCoreApplication app(argc,argv);QDir::setCurrent(WAFFLE_SOURCE_DIR);try{
 CodeplugDefinition d;QString e;check(CodeplugDefinition::load("definitions/cs750.wcp",&d,&e),e);
 QByteArray base=d.defaultCodeplug(),out;ImportedCodeplug p,q;
 check(d.read(base,&p,&e),e);check(p.powerSettings==QStringList({"low","high","medium"}),"definition power choices");
 check(d.write(base,p,p.radioName,p.radioId,&out,&e),e);check(out==base,"unchanged bytes");
 auto added=p.channels.last();added.name="MEDIUM TEST";added.sourceRecord=-1;added.useDefaultTemplate=true;added.setPowerValue("medium");added.contactIndex=2;
 p.contacts.append({"Added contact",1234567,true});p.channels.append(added);p.zones[0].channelIndexes.append(2);
 check(d.write(base,p,p.radioName,p.radioId,&out,&e),e);check(d.read(out,&q,&e),e);
 check(q.channels.size()==3 && q.contacts.size()==2 && q.channels.last().powerValue()=="medium","add records/medium");
 check(out.size()==base.size()+16,"contact tail grew");
 auto removed=q;removed.channels.removeFirst();for(auto &z:removed.zones){QVector<int> members;for(int n:z.channelIndexes)if(n>0)members.append(n-1);z.channelIndexes=members;}
 removed.contacts.removeFirst();for(auto &c:removed.channels)c.contactIndex=c.contactIndex>1?c.contactIndex-1:0;
 QByteArray shrunk;check(d.write(out,removed,removed.radioName,removed.radioId,&shrunk,&e),e);ImportedCodeplug again;check(d.read(shrunk,&again,&e),e);
 check(again.contacts.size()==1 && again.channels.size()==2 && again.channels.last().powerValue()=="medium","delete records");
 check(shrunk.size()==base.size(),"contact tail shrank");
 auto invalid=p;invalid.channels.last().setPowerValue("unsupported");QByteArray sentinel="unchanged";check(!d.write(base,invalid,p.radioName,p.radioId,&sentinel,&e),"unsupported power accepted");check(sentinel=="unchanged","failure mutated output");
 QTemporaryDir dir;check(dir.isValid(),"temp dir");QFile input(dir.filePath("power.rdb"));check(input.open(QIODevice::WriteOnly),"input");input.write(out);input.close();
 RepeaterStore store;check(store.importCodeplugFromFile(input.fileName(),"cs750.wcp"),"store import");check(store.powerChoices().contains("Medium"),"Medium UI choice");
QString project=dir.filePath("power.waffle");check(store.saveProjectToFile(project),"save project");RepeaterStore reopened;check(reopened.openProjectFromFile(project),"reopen project");
 check(reopened.powerChoices().contains("Medium"),"choice declarations restored");
 reopened.selectImportedGroup(0);
 check(reopened.updateSelectedZoneChannel(3, "MEDIUM TEST", "400.000000", "400.000000", true, 1, 2, 2, reopened.powerChoice("Low")), "edit Medium to Low");
 check(reopened.updateSelectedZoneChannel(3, "MEDIUM TEST", "400.000000", "400.000000", true, 1, 2, 2, reopened.powerChoice("Medium")), "edit Low to Medium");auto bytes=reopened.exportCodeplugBytes();check(!bytes.isEmpty(),"project export");check(d.read(bytes,&again,&e),e);check(again.channels.last().powerValue()=="medium","project lost Medium");
 // An arbitrary third label verifies that the parser does not special-case Medium.
 QMap<QString,QByteArray> entries;check(ZipArchive::read("definitions/cs750.wcp", &entries, &e), e);
 auto json=QJsonDocument::fromJson(entries["definition.json"]).object();auto tables=json["tables"].toObject();auto channels=tables["channels"].toObject();auto fields=channels["fields"].toObject();auto power=fields["power"].toObject();auto values=power["enum"].toObject();values["1"]="boost";power["enum"]=values;fields["power"]=power;channels["fields"]=fields;tables["channels"]=channels;json["tables"]=tables;entries["definition.json"]=QJsonDocument(json).toJson();
 QString custom=dir.filePath("custom.wcp");check(ZipArchive::write(custom, entries, &e),e);CodeplugDefinition arbitrary;check(CodeplugDefinition::load(custom,&arbitrary,&e),e);check(arbitrary.read(out,&again,&e),e);check(again.powerSettings.contains("boost") && again.channels.last().powerValue()=="boost", "arbitrary definition power label");
 if(argc>1 && QByteArray(argv[1])=="--samples"){
  QFile f("Codeplugs/CS750-HighPower.rdb");check(f.open(QIODevice::ReadOnly),"sample");auto src=f.readAll();check(d.read(src,&p,&e),e);check(p.contacts.size()==35041,"full contact import");
  check(d.write(src,p,p.radioName,p.radioId,&out,&e),e);check(out==src,"full unchanged");
  std::reverse(p.contacts.begin(),p.contacts.end());for(auto &c:p.channels)if(c.contactIndex)c.contactIndex=p.contacts.size()+1-c.contactIndex;
  p.channels[0].name="WAFFLE TEST";p.channels[0].setPowerValue("medium");p.zones.removeAt(29);
  check(d.write(src,p,p.radioName,p.radioId,&out,&e),e);check(d.read(out,&again,&e),e);check(again.channels[0].powerValue()=="medium" && again.zones.size()==74,"full edits");
  QFile artifact(dir.filePath("edited.rdb"));check(artifact.open(QIODevice::WriteOnly),"artifact");artifact.write(out);
 }
 qInfo()<<"CS750 definition, add/delete, reference validation, power choices and project roundtrip passed";return 0;
}catch(const std::exception &ex){qCritical()<<ex.what();return 1;}}
