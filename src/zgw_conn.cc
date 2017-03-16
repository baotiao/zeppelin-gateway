#include <sys/time.h>

#include "zgw_conn.h"
#include "zgw_server.h"
#include "zgw_auth.h"
#include "zgw_xml.h"
#include "libzgw/zgw_namelist.h"

#include "slash_hash.h"

extern ZgwServer* g_zgw_server;

static void DumpHttpRequest(const pink::HttpRequest* req) {
  DLOG(INFO) << "handle get"<< std::endl;
  DLOG(INFO) << " + method: " << req->method;
  DLOG(INFO) << " + path: " << req->path;
  DLOG(INFO) << " + version: " << req->version;
  if (req->content.size() > 50) {
    DLOG(INFO) << " + content: " << req->content.substr(0, 50);
  } else {
    DLOG(INFO) << " + content: " << req->content;
  }
  DLOG(INFO) << " + headers: ";
  DLOG(INFO) << "------------------------------------- ";
  for (auto h : req->headers) {
    DLOG(INFO) << "   + " << h.first << "=>" << h.second;
  }
  DLOG(INFO) << "------------------------------------- ";
  DLOG(INFO) << "------------------------------------- ";
  DLOG(INFO) << " + query_params: ";
  for (auto q : req->query_params) {
    DLOG(INFO) << "   + " << q.first << "=>" << q.second;
  }
  DLOG(INFO) << "------------------------------------- ";
} 

ZgwConn::ZgwConn(const int fd,
                 const std::string &ip_port,
                 pink::Thread* worker)
      : HttpConn(fd, ip_port) {
  worker_ = reinterpret_cast<ZgwWorkerThread *>(worker);
  store_ = worker_->GetStore();
}

std::string ZgwConn::GetAccessKey() {
  if (!req_->query_params["X-Amz-Credential"].empty()) {
    std::string credential_str = req_->query_params["X-Amz-Credential"];
    return credential_str.substr(0, 20);
  } else {
    std::string auth_str;
    auto iter = req_->headers.find("authorization");
    if (iter != req_->headers.end())
      auth_str.assign(iter->second);
    else return "";
    size_t pos = auth_str.find("Credential");
    if (pos == std::string::npos)
      return "";
    size_t slash_pos = auth_str.find('/');
    // e.g. auth_str: "...Credential=f3oiCCuyE7v3dOZgeEBsa/20170225/us..."
    return auth_str.substr(pos + 11, slash_pos - pos - 11);
  }
  return "";
}

void ZgwConn::DealMessage(const pink::HttpRequest* req, pink::HttpResponse* resp) {
  // DumpHttpRequest(req);

  // Copy req and resp
  req_ = const_cast<pink::HttpRequest *>(req);
  resp_ = resp;

  // Get bucket name and object name
  const std::string path = req_->path;
  if (path[0] != '/') {
    resp_->SetStatusCode(500);
    return;
  }
  size_t pos = path.find('/', 1);
  if (pos == std::string::npos) {
    bucket_name_.assign(path.substr(1));
    object_name_.clear();
  } else {
    bucket_name_.assign(path.substr(1, pos - 1));
    object_name_.assign(path.substr(pos + 1));
  }
  if (object_name_.back() == '/') {
    object_name_.resize(object_name_.size() - 1);
  }

  // Users operation, without authorization for now
  if (req_->method == "GET" &&
      bucket_name_ == "admin_list_users") {
    ListUsersHandle();
    return;
  } else if (req_->method == "PUT" &&
             bucket_name_ == "admin_put_user") {
    if (object_name_.empty()) {
      resp_->SetStatusCode(400);
      return;
    }
    std::string access_key;
    std::string secret_key;
    Status s = store_->AddUser(object_name_, &access_key, &secret_key);
    if (!s.ok()) {
      resp_->SetStatusCode(500);
      resp_->SetBody(s.ToString());
    } else {
      resp_->SetStatusCode(200);
      resp_->SetBody(access_key + "\r\n" + secret_key);
    }
    return;
  }
  
  // Get access key
  access_key_ = GetAccessKey();
  // Authorize access key
  Status s = store_->GetUser(access_key_, &zgw_user_);
  ZgwAuth zgw_auth;
  if (!s.ok()) {
    resp_->SetStatusCode(403);
    resp_->SetBody(xml::ErrorXml(xml::InvalidAccessKeyId, ""));
    return;
  }

  // TODO (gaodq) disable request authorization
  // Authorize request
  // if (!zgw_auth.Auth(req_, zgw_user_->secret_key(access_key_))) {
  //   resp_->SetStatusCode(403);
  //   resp_->SetBody(xml::ErrorXml(xml::SignatureDoesNotMatch, ""));
  //   return;
  // }

  // Get buckets namelist and ref
  auto const &info = zgw_user_->user_info();
  s = g_zgw_server->buckets_list()->Ref(store_, info.disply_name, &buckets_name_);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "List buckets name list failed: " << s.ToString();
    s = g_zgw_server->buckets_list()->Unref(store_, info.disply_name);
    return;
  }

  if (!bucket_name_.empty() && buckets_name_->IsExist(bucket_name_)) {
    // Get objects namelist and ref
    s = g_zgw_server->objects_list()->Ref(store_, bucket_name_, &objects_name_);
    if (!s.ok()) {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "List objects name list failed: " << s.ToString();
      s = g_zgw_server->buckets_list()->Unref(store_, info.disply_name);
      return;
    }
  }

  METHOD method;
  if (req_->method == "GET") {
    method = GET;
  } else if (req_->method == "PUT") {
    method = PUT;
  } else if (req_->method == "DELETE") {
    method = DELETE;
  } else if (req_->method == "HEAD") {
    method = HEAD;
  } else if (req_->method == "POST") {
    method = POST;
  } else {
    method = UNSUPPORT;
  }

  if (bucket_name_.empty()) {
    ListBucketHandle();
  } else if (IsBucketOp()) {
    switch(method) {
      case GET:
        if (req_->query_params.find("uploads") != req_->query_params.end()) {
          ListMultiPartsUpload();
        } else {
          ListObjectHandle();
        }
        break;
      case PUT:
        PutBucketHandle();
        break;
      case DELETE:
        DelBucketHandle();
        break;
      case HEAD:
        if (!buckets_name_->IsExist(bucket_name_)) {
          resp_->SetStatusCode(404);
        } else {
          resp_->SetStatusCode(200);
        }
        break;
      default:
        break;
    }
  } else if (IsObjectOp()) {
    // Check whether bucket existed in namelist meta
    if (!buckets_name_->IsExist(bucket_name_)) {
      resp_->SetStatusCode(404);
      resp_->SetBody(xml::ErrorXml(xml::NoSuchBucket, bucket_name_));
    } else {
      DLOG(INFO) << "Object Op: " << req_->path << " confirm bucket exist";
      g_zgw_server->object_mutex()->Lock(bucket_name_ + object_name_);
      switch(method) {
        case GET:
          if (req_->query_params.find("uploadId") != req_->query_params.end()) {
            ListParts(req_->query_params["uploadId"]);
          } else {
            GetObjectHandle();
          }
          break;
        case PUT:
          if (req_->query_params.find("partNumber") != req_->query_params.end() &&
              req_->query_params.find("uploadId") != req_->query_params.end()) {
            UploadPartHandle(req_->query_params["partNumber"],
                             req_->query_params["uploadId"]);
          } else {
            PutObjectHandle();
          }
          break;
        case DELETE:
          if (req_->query_params.find("uploadId") != req_->query_params.end()) {
            AbortMultiUpload(req_->query_params["uploadId"]);
          } else {
            DelObjectHandle();
          }
          break;
        case HEAD:
          GetObjectHandle(true);
          break;
        case POST:
          if (req_->query_params.find("uploads") != req_->query_params.end()) {
            InitialMultiUpload();
          } else if (req_->query_params.find("uploadId") != req_->query_params.end()) {
            CompleteMultiUpload(req_->query_params["uploadId"]);
          }
          break;
        default:
          break;
      }
      g_zgw_server->object_mutex()->Unlock(bucket_name_ + object_name_);
    }
  } else {
    // Unknow request
    resp_->SetStatusCode(501);
    resp_->SetBody(xml::ErrorXml(xml::NotImplemented, ""));
  }

  // Unref namelist
  Status s1 = Status::OK();
  s = g_zgw_server->buckets_list()->Unref(store_, info.disply_name);
  if (!bucket_name_.empty()) {
    s1 = g_zgw_server->objects_list()->Unref(store_, bucket_name_);
  }
  if (!s.ok() || !s1.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Unref namelist failed: " << s.ToString();
    return;
  }

  resp_->SetHeaders("Last-Modified", http_nowtime());
  resp_->SetHeaders("Date", http_nowtime());
}

void ZgwConn::InitialMultiUpload() {
  std::string upload_id, internal_obname;

  timeval now;
  gettimeofday(&now, NULL);
  libzgw::ZgwObjectInfo ob_info(now, "", 0, libzgw::kStandard, zgw_user_->user_info());
  std::string tmp_obname = object_name_ + std::to_string(time(NULL));
  upload_id.assign(slash::md5(tmp_obname));
  internal_obname.assign("__" + object_name_ + upload_id);

  Status s = store_->AddObject(bucket_name_, internal_obname, ob_info, "");
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    return;
  }
  DLOG(INFO) << "Get upload id, and insert multiupload meta to zp";

  // Insert into namelist
  objects_name_->Insert(internal_obname);
  DLOG(INFO) << "Insert into namelist: " << internal_obname;

  // Success Response
  resp_->SetBody(xml::InitiateMultipartUploadResultXml(bucket_name_,
                                                       object_name_, upload_id));
  resp_->SetStatusCode(200);
}

void ZgwConn::UploadPartHandle(const std::string& part_num, const std::string& upload_id) {
  std::string internal_obname = "__" + object_name_ + upload_id;
  if (!objects_name_->IsExist(internal_obname)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchUpload, upload_id));
    return;
  }

  std::string etag = "\"" + slash::md5(req_->content) + "\"";
  timeval now;
  gettimeofday(&now, NULL);
  libzgw::ZgwObjectInfo ob_info(now, etag, req_->content.size(), libzgw::kStandard,
                                zgw_user_->user_info());
  Status s = store_->UploadPart(bucket_name_, internal_obname, ob_info, req_->content,
                                std::stoi(part_num));
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "UploadPart data failed: " << s.ToString();
    return;
  }
  DLOG(INFO) << "UploadPart: " << req_->path << " confirm add to zp success";

  resp_->SetHeaders("ETag", etag);
  resp_->SetStatusCode(200);
}

void ZgwConn::CompleteMultiUpload(const std::string& upload_id) {
  std::string internal_obname = "__" + object_name_ + upload_id;
  if (!objects_name_->IsExist(internal_obname)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchUpload, upload_id));
    return;
  }
  DLOG(INFO) << "CompleteMultiUpload: " << req_->path << " confirm upload id exist";

  Status s;
  if (objects_name_->IsExist(object_name_)) {
    s = store_->DelObject(bucket_name_, object_name_);
    if (!s.ok()) {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "CompleteMultiUpload failed: " << s.ToString();
      return;
    }
  }
  DLOG(INFO) << "CompleteMultiUpload: " << req_->path << " confirm delete old object";

  s = store_->CompleteMultiUpload(bucket_name_, internal_obname);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "CompleteMultiUpload failed: " << s.ToString();
    return;
  }
  DLOG(INFO) << "CompleteMultiUpload: " << req_->path << " confirm zp's objects change name";

  objects_name_->Insert(object_name_);
  objects_name_->Delete(internal_obname);

  resp_->SetStatusCode(200);
}

void ZgwConn::AbortMultiUpload(const std::string& upload_id) {
  std::string internal_obname = "__" + object_name_ + upload_id;
  if (!objects_name_->IsExist(internal_obname)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchUpload, upload_id));
    return;
  }

  Status s = store_->DelObject(bucket_name_, internal_obname);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      // But founded in list meta, continue to delete from list meta
    } else {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "AbortMultiUpload failed: " << s.ToString();
      return;
    }
  }

  objects_name_->Delete(internal_obname);
  DLOG(INFO) << "AbortMultiUpload: " << req_->path << " confirm delete object meta from namelist success";

  // Success
  resp_->SetStatusCode(204);
}

void ZgwConn::ListParts(const std::string& upload_id) {
  std::string internal_obname = "__" + object_name_ + upload_id;
  if (!objects_name_->IsExist(internal_obname)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchUpload, upload_id));
    return;
  }
  DLOG(INFO) << "ListParts: " << req_->path << " confirm upload exist";

  std::vector<libzgw::ZgwObject> parts;
  Status s = store_->ListParts(bucket_name_, internal_obname, &parts);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "UploadPart data failed: " << s.ToString();
    return;
  }

  std::map<std::string, std::string> args{
    {"Bucket", bucket_name_},
    {"Key", object_name_},
    {"UploadId", upload_id},
    {"StorageClass", "STANDARD"},
    // {"PartNumberMarker", "1"},
    // {"NextPartNumberMarker", ""},
    {"MaxParts", "1000"},
    {"IsTruncated", "false"},
  };
  resp_->SetStatusCode(200);
  resp_->SetBody(xml::ListPartsResultXml(parts, zgw_user_, args));
}

void ZgwConn::ListMultiPartsUpload() {
  // Check whether bucket existed in namelist meta
  if (!buckets_name_->IsExist(bucket_name_)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchBucket, bucket_name_));
    return;
  }
  DLOG(INFO) << "ListMultiPartsUpload: " << req_->path << " confirm bucket exist";

  Status s;
  std::vector<libzgw::ZgwObject> objects;
  {
    std::lock_guard<std::mutex> lock(objects_name_->list_lock);
    for (auto &name : objects_name_->name_list) {
      if (name.find_first_of("__") != 0) {
        continue;
      }
      libzgw::ZgwObject object(bucket_name_, name);
      s = store_->GetObject(&object, false);
      if (!s.ok()) {
        if (s.IsNotFound()) {
          continue;
        }
        resp_->SetStatusCode(500);
        LOG(ERROR) << "ListMultiPartsUpload failed: " << s.ToString();
        return;
      }
      objects.push_back(object);
    }
  }

  std::string next_key_marker = objects.empty() ? "" : objects.back().name();
  std::map<std::string, std::string> args {
    {"Bucket", bucket_name_},
    {"NextKeyMarker", next_key_marker.empty() ? "" :
      next_key_marker.substr(2, next_key_marker.size() - 32 -2)},
    {"NextUploadIdMarker", objects.empty() ? "" : objects.back().upload_id()},
    {"MaxUploads", "1000"},
    {"IsTruncated", "false"},
  };
  resp_->SetStatusCode(200);
  resp_->SetBody(xml::ListMultipartUploadsResultXml(objects, args));
}

void ZgwConn::ListUsersHandle() {
  std::set<libzgw::ZgwUser *> user_list; // name : keys
  Status s = store_->ListUsers(&user_list);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    resp_->SetBody(s.ToString());
  } else {
    resp_->SetStatusCode(200);
    std::string body;
    for (auto &user : user_list) {
      const auto &info = user->user_info();
      body.append("disply_name: " + info.disply_name + "\r\n");

      for (auto &key_pair : user->access_keys()) {
        body.append(key_pair.first + "\r\n"); // access key
        body.append(key_pair.second + "\r\n"); // secret key
      }
      body.append("\r\n");
    }
    resp_->SetBody(body);
  }
}

void ZgwConn::DelObjectHandle() {
  DLOG(INFO) << "DeleteObject: " << bucket_name_ << "/" << object_name_;

  // Check whether object existed in namelist meta
  if (!objects_name_->IsExist(object_name_)) {
    resp_->SetStatusCode(204);
    return;
  }
  DLOG(INFO) << "DelObject: " << req_->path << " confirm object exist";

  // Delete object
  Status s = store_->DelObject(bucket_name_, object_name_);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      // But founded in list meta, continue to delete from list meta
    } else {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "Delete object data failed: " << s.ToString();
      return;
    }
  }
  DLOG(INFO) << "DelObject: " << req_->path << " confirm delete object from zp success";

  // Delete from list meta
  objects_name_->Delete(object_name_);

  DLOG(INFO) << "DelObject: " << req_->path << " confirm delete object meta from namelist success";

  // Success
  resp_->SetStatusCode(204);
}

void ZgwConn::GetObjectHandle(bool is_head_op) {
  DLOG(INFO) << "GetObjects: " << bucket_name_ << "/" << object_name_;

  if (!objects_name_->IsExist(object_name_)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchKey, object_name_));
    return;
  }
  DLOG(INFO) << "GetObject: " << req_->path << " confirm object exist";

  // Get object
  libzgw::ZgwObject object(bucket_name_, object_name_);
  bool need_content = !is_head_op;
  Status s = store_->GetObject(&object, need_content);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Get object data failed: " << s.ToString();
    return;
  }
  DLOG(INFO) << "GetObject: " << req_->path << " confirm get object from zp success";

  resp_->SetBody(object.content());
  resp_->SetStatusCode(200);
}

void ZgwConn::PutObjectHandle() {
  DLOG(INFO) << "PutObjcet: " << bucket_name_ << "/" << object_name_;

  // Put object data
  std::string etag = "\"" + slash::md5(req_->content) + "\"";
  timeval now;
  gettimeofday(&now, NULL);
  libzgw::ZgwObjectInfo ob_info(now, etag, req_->content.size(), libzgw::kStandard,
                                zgw_user_->user_info());
  Status s = store_->AddObject(bucket_name_, object_name_, ob_info, req_->content);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Put object data failed: " << s.ToString();
    return;
  }
  DLOG(INFO) << "PutObject: " << req_->path << " confirm add to zp success";

  // Put object to list meta
  objects_name_->Insert(object_name_);

  DLOG(INFO) << "PutObject: " << req_->path << " confirm add to namelist success";

  resp_->SetHeaders("ETag", etag);
  resp_->SetStatusCode(200);
}

void ZgwConn::ListObjectHandle() {
  DLOG(INFO) << "ListObjects: " << bucket_name_;

  // Check whether bucket existed in namelist meta
  if (!buckets_name_->IsExist(bucket_name_)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchBucket, bucket_name_));
    return;
  }
  DLOG(INFO) << "ListObjects: " << req_->path << " confirm bucket exist";

  // Get objects meta from zp
  Status s;
  std::vector<libzgw::ZgwObject> objects;
  {
    std::lock_guard<std::mutex> lock(objects_name_->list_lock);
    for (auto &name : objects_name_->name_list) {
      libzgw::ZgwObject object(bucket_name_, name);
      s = store_->GetObject(&object, false);
      if (!s.ok()) {
        if (s.IsNotFound()) {
          continue;
        }
        resp_->SetStatusCode(500);
        LOG(ERROR) << "ListObjects failed: " << s.ToString();
        return;
      }
      objects.push_back(object);
    }
  }
  DLOG(INFO) << "ListObjects: " << req_->path << " confirm get objects' meta from zp success";

  // Success Http response
  std::map<std::string, std::string> args{
    {"Name", bucket_name_},
    {"MaxKeys", "1000"},
    {"IsTruncated", "false"},
  };
  // args["Name"] = bucket_name_;
  // args["MaxKeys"] = "1000";
  // args["IsTruncated"] = "false";
  resp_->SetBody(xml::ListObjectsXml(objects, args));
  resp_->SetStatusCode(200);
}

void ZgwConn::DelBucketHandle() {
  DLOG(INFO) << "DeleteBucket: " << bucket_name_;
  // Check whether bucket existed in namelist meta
  if (!buckets_name_->IsExist(bucket_name_)) {
    resp_->SetStatusCode(404);
    resp_->SetBody(xml::ErrorXml(xml::NoSuchBucket, bucket_name_));
    return;
  }
  DLOG(INFO) << "DeleteBucket: " << req_->path << " confirm bucket exist";
  // Need not check return value

  if (objects_name_ == NULL || !objects_name_->IsEmpty()) {
    resp_->SetStatusCode(409);
    resp_->SetBody(xml::ErrorXml(xml::BucketNotEmpty, bucket_name_));
    LOG(ERROR) << "DeleteBucket: BucketNotEmpty";
    return;
  }

  Status s = store_->DelBucket(bucket_name_);
  if (s.ok()) {
    buckets_name_->Delete(bucket_name_);
    resp_->SetStatusCode(204);
  } else if (s.IsIOError()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Delete bucket failed: " << s.ToString();
  }

  DLOG(INFO) << "DelBucket: " << req_->path << " confirm delete from namelist success";
}

void ZgwConn::PutBucketHandle() {
  DLOG(INFO) << "CreateBucket: " << bucket_name_;

  // Check whether bucket existed in namelist meta
  if (buckets_name_->IsExist(bucket_name_)) {
    resp_->SetStatusCode(409);
    resp_->SetBody(xml::ErrorXml(xml::BucketAlreadyOwnedByYou, ""));
    return;
  }

  // Check whether belong to other user
  std::set<libzgw::ZgwUser *> user_list; // name : keys
  Status s = store_->ListUsers(&user_list);
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Create bucket failed: " << s.ToString();
  }
  libzgw::NameList *tmp_bk_list;
  bool already_exist = false;
  for (auto user : user_list) {
    const auto &info = user->user_info();
    s = g_zgw_server->buckets_list()->Ref(store_, info.disply_name, &tmp_bk_list);
    if (!s.ok()) {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "Create bucket failed: " << s.ToString();
      return;
    }
    if (tmp_bk_list->IsExist(bucket_name_)) {
      already_exist = true;
    }
    s = g_zgw_server->buckets_list()->Unref(store_, info.disply_name);
    if (!s.ok()) {
      resp_->SetStatusCode(500);
      LOG(ERROR) << "Create bucket failed: " << s.ToString();
      return;
    }
    if (already_exist) {
      resp_->SetStatusCode(409);
      resp_->SetBody(xml::ErrorXml(xml::BucketAlreadyExists, ""));
      return;
    }
  }

  DLOG(INFO) << "ListObjects: " << req_->path << " confirm bucket not exist";

  // Create bucket in zp
  s = store_->AddBucket(bucket_name_, zgw_user_->user_info());
  if (!s.ok()) {
    resp_->SetStatusCode(500);
    LOG(ERROR) << "Create bucket failed: " << s.ToString();
    return;
  }
  DLOG(INFO) << "PutBucket: " << req_->path << " confirm add bucket to zp success";

  // Create list meta info
  buckets_name_->Insert(bucket_name_);

  DLOG(INFO) << "PutBucket: " << req_->path << " confirm add bucket to namelist success";

  // Success
  resp_->SetStatusCode(200);
}

void ZgwConn::ListBucketHandle() {
  DLOG(INFO) << "ListBuckets: ";
  // Find object list meta

  // Load bucket info from zp
  Status s;
  std::vector<libzgw::ZgwBucket> buckets;
  {
    std::lock_guard<std::mutex> lock(buckets_name_->list_lock);
    for (auto &name : buckets_name_->name_list) {
      libzgw::ZgwBucket bucket(name);
      s = store_->GetBucket(&bucket);
      if (!s.ok()) {
        if (s.IsNotFound()) {
          continue;
        }
        resp_->SetStatusCode(500);
        LOG(ERROR) << "ListBuckets failed: " << s.ToString();
        return;
      }
      buckets.push_back(bucket);
    }
  }

  // Zeppelin success, then build http body

  const libzgw::ZgwUserInfo &info = zgw_user_->user_info();
  resp_->SetStatusCode(200);
  resp_->SetBody(xml::ListBucketXml(info, buckets));
}

std::string ZgwConn::http_nowtime() {
  char buf[100] = {0};
  time_t now = time(0);
  struct tm t = *gmtime(&now);
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %Z", &t);
  return std::string(buf);
}
