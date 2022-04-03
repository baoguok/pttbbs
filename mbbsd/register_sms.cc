extern "C" {
#include "bbs.h"
#include "daemons.h"

void u_sms_verification();
}

#ifdef USE_SMS_VERIFICATION

#include <vector>

#include "flatbuffers/flatbuffers.h"
#include "register_sms.fbs.h"
#include "verifydb.fbs.h"
#include "verifydb.h"

#define FN_SMS_AGREEMENT "etc/reg.sms.notes"

#define REGISTER_SMS_LOG(ev, fmt, ...) \
  log_filef("log/register_sms.log", LOG_CREAT, \
      "%ld %s %s %d %s " fmt "\n", \
      time(nullptr), (ev), \
      cuser.userid, cuser.firstlogin, fromhost, __VA_ARGS__)

namespace {

using Bytes = std::vector<uint8_t>;

const char *kCodePrefix = "ptt";
constexpr int kCodeLen = 3 + 8;
constexpr int kCodeValidSecs = 15 * 60;
constexpr int kMaxStateSize = 16384;
constexpr int kCooldownDays = 3;

struct UserRef {
  std::string userid;
  int64_t generation;

  static UserRef Current() {
    UserRef uref;
    uref.userid = cuser.userid;
    uref.generation = cuser.firstlogin;
    return uref;
  }
};

// Validates a Taiwan mobile phone number in local format (e.g. 0912345678), and
// returns the E.164 format (e.g. 886912345678). Note that the returned number
// does not have a plus ('+') sign.
bool ValidatePhone(const char *phone, std::string *e164) {
  // XXX: Hard code Taiwan's mobile phone number format.
  int i;
  for (i = 0; phone[i]; i++)
    if (!isdigit(phone[i]))
      return false;
  if (i != SMS_PHONE_NUMBER_LEN)
    return false;
  if (phone[0] != '0' || phone[1] != '9')
    return false;

  *e164 = "886";
  e164->append(phone + 1);
  return true;
}

std::string RandomCode() {
  uint32_t num;
  must_getrandom(&num, sizeof(num));
  num %= 100000000U;
  return std::string(kCodePrefix) + std::to_string(num);
}

class SmsValidation {
public:
  SmsValidation(UserRef uref) : uref_(uref) {}

  void Run();

private:
  UserRef uref_;
  std::string session_id_;
  std::string phone_;
  std::string code_;
  time_t state_timestamp_ = 0;

  bool LoadState();
  bool SaveState();
  void ResetState();

  bool InputPhone();
  bool InputCode();
  bool CompleteVerification();

  bool InsertSession();

  SmsValidation(const SmsValidation&) = delete;
  SmsValidation& operator=(const SmsValidation&) = delete;

  static std::string StatePath(const UserRef &uref);
};

// static
std::string SmsValidation::StatePath(const UserRef &uref) {
  std::string path = std::string(BBSHOME "/jobspool/sms.");

  size_t off = path.size();
  path += uref.userid;
  std::transform(path.begin() + off, path.end(), path.begin() + off,
                 [](char c) { return std::tolower(c); });

  path += ".";
  path += std::to_string(uref.generation);
  return path;
}

bool SmsValidation::LoadState() {
  auto path = StatePath(uref_);
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno != ENOENT)
      unlink(path.c_str());
    return false;
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size > kMaxStateSize) {
    close(fd);
    unlink(path.c_str());
    return false;
  }

  Bytes buf;
  buf.resize(st.st_size);
  if (read(fd, &buf.front(), buf.size()) != static_cast<ssize_t>(buf.size())) {
    close(fd);
    unlink(path.c_str());
    return false;
  }
  close(fd);

  auto verifier = flatbuffers::Verifier(buf.data(), buf.size());
  if (!RegisterSms::VerifyStateBuffer(verifier)) {
    unlink(path.c_str());
    return false;
  }

  auto state = RegisterSms::GetState(buf.data());
  state_timestamp_ = state->timestamp();
  session_id_ = state->session_id()->str();
  phone_ = state->phone()->str();
  code_ = state->code()->str();
  return true;
}

bool SmsValidation::SaveState() {
  state_timestamp_ = time(nullptr);

  flatbuffers::FlatBufferBuilder fbb;
  auto state = RegisterSms::CreateStateDirect(
      fbb, uref_.userid.c_str(), uref_.generation, state_timestamp_,
      session_id_.c_str(), phone_.c_str(), code_.c_str());
  fbb.Finish(state);

  auto path = StatePath(uref_);
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;
  if (write(fd, fbb.GetBufferPointer(), fbb.GetSize()) != fbb.GetSize()) {
    close(fd);
    unlink(path.c_str());
    return false;
  }
  close(fd);
  return true;
}

void SmsValidation::ResetState() {
  session_id_.clear();
  phone_.clear();
  code_.clear();
  state_timestamp_ = 0;
  unlink(StatePath(uref_).c_str());
}

bool SmsValidation::InputPhone() {
  std::string fullphone;
  while (1) {
    // User phone number input.
    char phone[SMS_PHONE_NUMBER_LEN + 1] = {};
    move(4, 0);
    outs("�ثe�ȱ����x�W��a��������X�A�H 09 �}�Y�@�Q�X�C");
    getdata(3, 0, "�п�J������X: ", phone, sizeof(phone), LCECHO);
    if (!phone[0])
      return false;
    if (!ValidatePhone(phone, &fullphone)) {
      vmsg("������X�����T�C");
      continue;
    }

    // Check DB if it's already used.
    Bytes buf;
    const VerifyDb::GetReply *reply;
    if (!verifydb_getverify(VMETHOD_SMS, fullphone.c_str(), &buf, &reply) ||
        !reply->ok() || !reply->entry()) {
      vmsg("�{�Ҩt�εL�k�ϥΡA�еy�ԦA�աC");
      return false;
    }
    if (reply->entry()->size() > 0) {
      vmsg("�����X�w�Q�ϥιL�F�I");
      continue;
    }
    break;
  }
  phone_ = fullphone;
  return true;
}

bool SmsValidation::CompleteVerification() {
  assert(phone_.size() > 0);

  REGISTER_SMS_LOG("BEGIN_VERIFY_WRITE", "%s", phone_.c_str());

  // Check DB if it's already used.
  Bytes buf;
  const VerifyDb::GetReply *reply;
  if (!verifydb_getverify(VMETHOD_SMS, phone_.c_str(), &buf, &reply) ||
      !reply->ok() || !reply->entry()) {
    REGISTER_SMS_LOG("VERIFYDB_FAILED_CONN", "%s", phone_.c_str());
    vmsg("�{�Ҩt�εL�k�ϥΡA�еy�ԦA�աC");
    return false;
  }
  if (reply->entry()->size() > 0) {
    REGISTER_SMS_LOG("VERIFYDB_FAILED_DUP", "%s", phone_.c_str());
    vmsg("�@�Ӹ��X�u��{�Ҥ@�ӱb����I");
    return false;
  }
  // Write to DB.
  if (verifydb_set(uref_.userid.c_str(), uref_.generation, VMETHOD_SMS,
                   phone_.c_str(), 0) != 0) {
    REGISTER_SMS_LOG("VERIFYDB_WRITE_FAILED", "%s", phone_.c_str());
    vmsg("�{�Ҩt�εL�k�g�J�A�еy�ԦA�աC");
    return false;
  }
  REGISTER_SMS_LOG("VERIFYDB_WRITE_OK", "%s", phone_.c_str());

  // Remove the state file first, as we may exit.
  unlink(StatePath(uref_).c_str());

  if (cuser.userlevel & PERM_LOGINOK) {
    // Account aleady verified, just show a message.
    vmsg("�{�ҧ����I");
  } else {
    // Update PASSWD, send inner mail, and ask to relogin.
    char justify[sizeof(cuser.justify)];
    snprintf(justify, sizeof(justify), "<SMS>: %s", Cdate(&now));
    pwcuRegCompleteJustify(justify);

    register_mail_complete_and_exit();
  }
  return true;
}

bool SmsValidation::InputCode() {
  int tries = 10;
  do {
    // clang-format off
    move(10, 0);
    outs(ANSI_COLOR(1;33) "�`�N: " ANSI_RESET "�{�ҽX���� ");
    outs(kCodePrefix);
    outs(" �}�Y, ������� 8 ��Ʀr�C");
    // clang-format on
    char incode[kCodeLen + 1] = {};
    getdata(9, 0, "�п�J�{�ҽX: ", incode, sizeof(incode), LCECHO);
    if (std::string(incode) == code_)
      return true;
    if (--tries > 0)
      vmsgf("�{�ҽX���~! �٦� %d �����|.", tries);
    else
      vmsg("�{�ҽX���~���ƹL�h, �{�Ҩ���.");
    REGISTER_SMS_LOG("INVALID_CODE", "%s %s", code_.c_str(), incode);
  } while (tries);
  return false;
}

bool SmsValidation::InsertSession() {
  session_id_.clear();

  std::string data = "secret=";
  data.append(SMS_INSERT_SECRET);
  data.append("&phone=");
  data.append(phone_);
  data.append("&ip=");
  data.append(fromhost);
  data.append("&code=");
  data.append(code_);

  THTTP t;
  thttp_init(&t);
  int ret =
      thttp_post(&t, SMS_INSERT_SERVER_ADDR, SMS_INSERT_URI, SMS_INSERT_HOST,
                 "application/x-www-form-urlencoded", data.data(), data.size());
  if (ret == 0 && thttp_code(&t) == 200) {
    const char *sid;
    int sid_len = thttp_get_header(&t, "X-Aotp-Session-Id", &sid);
    if (sid_len > 0)
      session_id_.assign(sid, sid_len);
  }
  thttp_cleanup(&t);

  REGISTER_SMS_LOG(
      session_id_.empty() ? "NEW_SESSION_FAILED" : "NEW_SESSION_OK", "%s %s %s",
      phone_.c_str(), code_.c_str(), session_id_.c_str());

  return !session_id_.empty();
}

void SmsValidation::Run() {
  if (!verifydb_check_vmethod_unused(cuser.userid, cuser.firstlogin,
                                     VMETHOD_SMS))
    return;
  if (cuser.userlevel & PERM_NOREGCODE) {
    vmsg("�z���Q���\\�ϥλ{�ҽX�{�ҡC");
    return;
  }

  // Check if user had tried before.
  bool is_resume = false;
  if (LoadState()) {
    time_t passed = time(nullptr) - state_timestamp_;
    if (passed > kCooldownDays * 86400) {
      ResetState();
    } else if (passed > kCodeValidSecs) {
      vmsgf("�z�y�����{�ҽX�w�g�L��, �C %d �Ѷȯ���դ@��", kCooldownDays);
      return;
    } else if (vans("�z�y���w���չL�{��, �z�O�_�w���{�ҽX? [y/N] ") != 'y') {
      vmsgf("�C %d �ѥi�H���ջ{�Ҥ@��, �z�W�����լ� %ld �ѫe.", kCooldownDays,
            passed / 86400);
      return;
    } else {
      is_resume = true;
    }
  }

  // Show user agreement for a new session.
  if (!is_resume) {
    if (more(FN_SMS_AGREEMENT, YEA) < 0) {
      // We must have an user agreement for SMS.
      vmsg("�t�ο��~�A�Ц� " BN_BUGREPORT " �ݪO�^��");
      return;
    }
    if (vans("�аݱz�����ϥα��ڶ�? (y/N) ") != 'y') {
      vmsg("�ާ@����");
      return;
    }
  }

  vs_hdr("����{��");

  if (!is_resume) {
    if (!InputPhone())
      return;

    code_ = RandomCode();
    if (!InsertSession() || !SaveState()) {
      vmsg("�t�ο��~�A�Ц� " BN_BUGREPORT " �ݪO�^��");
      return;
    }

#ifdef SMS_URL
    move(6, 0);
    outs("�ЦܥH�U�s���i��{�Ҿާ@:\n");
    outs((std::string(SMS_URL) + "?id=" + session_id_).c_str());
    outs("\n");
#endif
  }

  if (!InputCode())
    return;

  CompleteVerification();
  // No code should go here. We may exit already.
}

}  // namespace

void u_sms_verification() {
  SmsValidation smsv(UserRef::Current());
  smsv.Run();
}

#endif // USE_SMS_VERIFICATION
