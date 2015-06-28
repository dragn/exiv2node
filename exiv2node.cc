#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <nan.h>
#include <unistd.h>
#include <string>
#include <map>
#include <list>
#include <exception>
#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/preview.hpp>

using namespace node;
using namespace v8;

// Create a map of strings for passing them back and forth between the V8 and
// worker threads.
typedef std::map<std::string, std::list<std::string> > tag_map_t;

class Exiv2Worker : public NanAsyncWorker {
 public:
  Exiv2Worker(NanCallback *callback, const std::string fileName)
    : NanAsyncWorker(callback), fileName(fileName) {}
  ~Exiv2Worker() {}

 protected:
  const std::string fileName;
  std::string exifException;
};

// - - -

class GetTagsWorker : public Exiv2Worker {
 public:
  GetTagsWorker(NanCallback *callback, const std::string fileName)
    : Exiv2Worker(callback, fileName) {}
  ~GetTagsWorker() {}

  // Should become protected...
  tag_map_t tags;

  // Executed inside the worker-thread. Not safe to access V8, or V8 data
  // structures here.
  void Execute () {
    try {
      Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(fileName);
      assert(image.get() != 0);
      image->readMetadata();

      Exiv2::ExifData &exifData = image->exifData();
      if (exifData.empty() == false) {
        Exiv2::ExifData::const_iterator end = exifData.end();
        for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
          tags[i->key()].push_back(i->value().toString());
        }
      }

      Exiv2::IptcData &iptcData = image->iptcData();
      if (iptcData.empty() == false) {
        Exiv2::IptcData::const_iterator end = iptcData.end();
        for (Exiv2::IptcData::const_iterator i = iptcData.begin(); i != end; ++i) {
          tags[i->key()].push_back(i->value().toString());
        }
      }

      Exiv2::XmpData &xmpData = image->xmpData();
      if (xmpData.empty() == false) {
        Exiv2::XmpData::const_iterator end = xmpData.end();
        for (Exiv2::XmpData::const_iterator i = xmpData.begin(); i != end; ++i) {
          tags[i->key()].push_back(i->value().toString());
        }
      }
    } catch (std::exception& e) {
      exifException.append(e.what());
    }
  }

  // This function will be run inside the main event loop so it is safe to use
  // V8 again.
  void HandleOKCallback () {
    NanScope();

    Local<Value> argv[2] = { NanNull(), NanNull() };

    if (!exifException.empty()){
      argv[0] = NanNew<String>(exifException.c_str());
    } else if (!tags.empty()) {
      Local<Object> hash = NanNew<Object>();
      // Copy the tags out.
      for (tag_map_t::iterator i = tags.begin(); i != tags.end(); ++i) {
        std::list<std::string> &value = i->second;
        if (value.size() > 1) {
          // insert array
          Local<Array> list = NanNew<Array>(value.size());
          uint32_t index = 0;
          for (std::list<std::string>::iterator j = value.begin(); j != value.end(); j++) {
            list->Set(index++, NanNew<String>(j->c_str()));
          }
          hash->Set(NanNew<String>(i->first.c_str()), list);
        } else {
          // insert string
          hash->Set(NanNew<String>(i->first.c_str()), NanNew<String>(value.front().c_str()));
        }
      }
      argv[1] = hash;
    }

    // Pass the argv array object to our callback function.
    callback->Call(2, argv);
  };
};

NAN_METHOD(GetImageTags) {
  NanScope();

  /* Usage arguments */
  if (args.Length() <= 1 || !args[1]->IsFunction())
    return NanThrowTypeError("Usage: <filename> <callback function>");

  NanCallback *callback = new NanCallback(args[1].As<Function>());
  std::string filename = std::string(*NanUtf8String(args[0]));

  NanAsyncQueueWorker(new GetTagsWorker(callback, filename));
  NanReturnUndefined();
}

// - - -

class SetTagsWorker : public Exiv2Worker {
 public:
  SetTagsWorker(NanCallback *callback, const std::string fileName)
    : Exiv2Worker(callback, fileName) {}
  ~SetTagsWorker() {}

  // Should become protected...
  tag_map_t tags;

  // Executed inside the worker-thread. Not safe to access V8, or V8 data
  // structures here.
  void Execute () {
    try {
      Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(fileName);
      assert(image.get() != 0);

      image->readMetadata();
      Exiv2::ExifData &exifData = image->exifData();
      Exiv2::IptcData &iptcData = image->iptcData();
      Exiv2::XmpData &xmpData = image->xmpData();

      exifData.sortByKey();
      iptcData.sortByKey();
      xmpData.sortByKey();

      // Assign the tags.
      for (tag_map_t::iterator i = tags.begin(); i != tags.end(); ++i) {
        const std::string &key = i->first;
        std::list<std::string> &val = i->second;
        if (val.size() > 1) {
          // multiple entries, replace all matching properties
          if (key.compare(0, 5, "Exif.") == 0) {
            Exiv2::ExifData::iterator it = exifData.findKey(Exiv2::ExifKey(key));
            while (it != exifData.end() && it->key() == key) it = exifData.erase(it);
            Exiv2::AsciiValue exivValue;
            for (std::list<std::string>::iterator l = val.begin(); l != val.end(); l++) {
              exivValue.read(*l);
              exifData.add(Exiv2::ExifKey(key), &exivValue);
            }
          } else if (key.compare(0, 5, "Iptc.") == 0) {
            Exiv2::IptcData::iterator it = iptcData.findKey(Exiv2::IptcKey(key));
            while (it != iptcData.end() && it->key() == key) it = iptcData.erase(it);
            Exiv2::AsciiValue exivValue;
            for (std::list<std::string>::iterator l = val.begin(); l != val.end(); l++) {
              exivValue.read(*l);
              iptcData.add(Exiv2::IptcKey(key), &exivValue);
            }
            iptcData[key].setValue(val.front());
          } else if (key.compare(0, 4, "Xmp.") == 0) {
            Exiv2::XmpData::iterator it = xmpData.findKey(Exiv2::XmpKey(key));
            while (it != xmpData.end() && it->key() == key) it = xmpData.erase(it);
            Exiv2::AsciiValue exivValue;
            for (std::list<std::string>::iterator l = val.begin(); l != val.end(); l++) {
              exivValue.read(*l);
              xmpData.add(Exiv2::XmpKey(key), &exivValue);
            }
          }
        } else {
          if (key.compare(0, 5, "Exif.") == 0) {
            exifData[key].setValue(val.front());
          } else if (key.compare(0, 5, "Iptc.") == 0) {
            iptcData[key].setValue(val.front());
          } else if (key.compare(0, 4, "Xmp.") == 0) {
            xmpData[key].setValue(val.front());
          } else {
            //std::cerr << "skipping unknown tag " << key << std::endl;
          }
        }
      }

      // Write the tag data the image file.
      image->setExifData(exifData);
      image->setIptcData(iptcData);
      image->setXmpData(xmpData);
      image->writeMetadata();
    } catch (std::exception& e) {
      exifException.append(e.what());
    }
  }

  // This function will be run inside the main event loop so it is safe to use
  // V8 again.
  void HandleOKCallback () {
    NanScope();

    // Create an argument array for any errors.
    Local<Value> argv[1] = { NanNull() };
    if (!exifException.empty()) {
      argv[0] = NanNew<String>(exifException.c_str());
    }

    // Pass the argv array object to our callback function.
    callback->Call(1, argv);
  };
};

NAN_METHOD(SetImageTags) {
  NanScope();

  /* Usage arguments */
  if (args.Length() <= 2 || !args[2]->IsFunction())
    return NanThrowTypeError("Usage: <filename> <tags> <callback function>");

  NanCallback *callback = new NanCallback(args[2].As<Function>());
  std::string filename = std::string(*NanUtf8String(args[0]));

  SetTagsWorker *worker = new SetTagsWorker(callback, filename);

  Local<Object> tags = Local<Object>::Cast(args[1]);
  Local<Array> keys = tags->GetPropertyNames();
  for (unsigned i = 0; i < keys->Length(); i++) {
    Handle<Value> key = keys->Get(i);
    if (tags->Get(key)->IsArray()) {
      Handle<Array> array = Handle<Array>::Cast(tags->Get(key));
      for (unsigned i = 0; i < array->Length(); i++) {
        worker->tags[*NanUtf8String(key)].push_back(*NanUtf8String(array->Get(i)));
      }
    } else {
      worker->tags[*NanUtf8String(key)].push_back(*NanUtf8String(tags->Get(key)));
    }
  }

  NanAsyncQueueWorker(worker);
  NanReturnUndefined();
}

// - - -

class DeleteTagsWorker : public Exiv2Worker {
 public:
  DeleteTagsWorker(NanCallback *callback, const std::string fileName)
    : Exiv2Worker(callback, fileName) {}
  ~DeleteTagsWorker() {}

  // Should become protected...
  std::vector<std::string> tags;

  // Executed inside the worker-thread. Not safe to access V8, or V8 data
  // structures here.
  void Execute () {
    try {
      Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(fileName);
      assert(image.get() != 0);

      image->readMetadata();
      Exiv2::ExifData &exifData = image->exifData();
      Exiv2::IptcData &iptcData = image->iptcData();
      Exiv2::XmpData &xmpData = image->xmpData();

      // Erase the tags.
      for (std::vector<std::string>::iterator i = tags.begin(); i != tags.end(); ++i) {
        if (i->compare(0, 5, "Exif.") == 0) {
          Exiv2::ExifKey *k = new Exiv2::ExifKey(*i);
          exifData.erase(exifData.findKey(*k));
          delete k;
        } else if (i->compare(0, 5, "Iptc.") == 0) {
          Exiv2::IptcKey *k = new Exiv2::IptcKey(*i);
          iptcData.erase(iptcData.findKey(*k));
          delete k;
        } else if (i->compare(0, 4, "Xmp.") == 0) {
          Exiv2::XmpKey *k = new Exiv2::XmpKey(*i);
          xmpData.erase(xmpData.findKey(*k));
          delete k;
        } else {
          //std::cerr << "skipping unknown tag " << i << std::endl;
        }
      }

      // Write the tag data the image file.
      image->setExifData(exifData);
      image->setIptcData(iptcData);
      image->setXmpData(xmpData);
      image->writeMetadata();
    } catch (std::exception& e) {
      exifException.append(e.what());
    }
  }

  // This function will be run inside the main event loop so it is safe to use
  // V8 again.
  void HandleOKCallback () {
    NanScope();

    // Create an argument array for any errors.
    Local<Value> argv[1] = { NanNull() };
    if (!exifException.empty()) {
      argv[0] = NanNew<String>(exifException.c_str());
    }

    // Pass the argv array object to our callback function.
    callback->Call(1, argv);
  };
};

NAN_METHOD(DeleteImageTags) {
  NanScope();

  /* Usage arguments */
  if (args.Length() <= 2 || !args[2]->IsFunction())
    return NanThrowTypeError("Usage: <filename> <tags> <callback function>");

  NanCallback *callback = new NanCallback(args[2].As<Function>());
  std::string filename = std::string(*NanUtf8String(args[0]));

  DeleteTagsWorker *worker = new DeleteTagsWorker(callback, filename);

  Local<Array> keys = Local<Array>::Cast(args[1]);
  for (unsigned i = 0; i < keys->Length(); i++) {
    Handle<v8::Value> key = keys->Get(i);
    worker->tags.push_back(*NanUtf8String(key));
  }

  NanAsyncQueueWorker(worker);
  NanReturnUndefined();
}

// - - -

class GetPreviewsWorker : public Exiv2Worker {
 public:
  GetPreviewsWorker(NanCallback *callback, const std::string fileName)
    : Exiv2Worker(callback, fileName) {}
  ~GetPreviewsWorker() {}

  // Executed inside the worker-thread. Not safe to access V8, or V8 data
  // structures here.
  void Execute () {
    try {
      Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(fileName);
      assert(image.get() != 0);
      image->readMetadata();

      Exiv2::PreviewManager manager(*image);
      Exiv2::PreviewPropertiesList list = manager.getPreviewProperties();

      // Make sure we're not reusing a baton with memory allocated.
      for (Exiv2::PreviewPropertiesList::iterator pos = list.begin(); pos != list.end(); pos++) {
        Exiv2::PreviewImage image = manager.getPreviewImage(*pos);

        previews.push_back(Preview(pos->mimeType_, pos->height_,
          pos->width_, (char*) image.pData(), pos->size_));
      }
    } catch (std::exception& e) {
      exifException.append(e.what());
    }
  }

  // This function will be run inside the main event loop so it is safe to use
  // V8 again.
  void HandleOKCallback () {
    NanScope();

    Local<Value> argv[2] = { NanNull(), NanNull() };
    if (!exifException.empty()){
      argv[0] = NanNew<String>(exifException.c_str());
    } else {
      // Convert the data into V8 values.
      Local<Array> array = NanNew<Array>(previews.size());
      for (size_t i = 0; i < previews.size(); ++i) {
        Local<Object> preview = NanNew<Object>();
        preview->Set(NanNew<String>("mimeType"), NanNew<String>(previews[i].mimeType.c_str()));
        preview->Set(NanNew<String>("height"), NanNew<Number>(previews[i].height));
        preview->Set(NanNew<String>("width"), NanNew<Number>(previews[i].width));
        preview->Set(NanNew<String>("data"), NanNewBufferHandle(previews[i].data, previews[i].size));

        array->Set(i, preview);
      }
      argv[1] = array;
    }

    // Pass the argv array object to our callback function.
    callback->Call(2, argv);
  };

 protected:

  // Holds a preview image when we copy it from the worker to V8.
  struct Preview {
    Preview(const Preview &p)
      : mimeType(p.mimeType), height(p.height), width(p.width), size(p.size)
    {
      data = new char[size];
      memcpy(data, p.data, size);
    }
    Preview(std::string type_, uint32_t height_, uint32_t width_, const char *data_, size_t size_)
      : mimeType(type_), height(height_), width(width_), size(size_)
    {
      data = new char[size];
      memcpy(data, data_, size);
    }
    ~Preview() {
      delete[] data;
    }

    std::string mimeType;
    uint32_t height;
    uint32_t width;
    size_t size;
    char* data;
  };

  std::vector<Preview> previews;
};

NAN_METHOD(GetImagePreviews) {
  NanScope();

  /* Usage arguments */
  if (args.Length() <= 1 || !args[1]->IsFunction())
    return NanThrowTypeError("Usage: <filename> <callback function>");

  NanCallback *callback = new NanCallback(args[1].As<Function>());
  std::string filename = std::string(*NanUtf8String(args[0]));

  NanAsyncQueueWorker(new GetPreviewsWorker(callback, filename));
  NanReturnUndefined();
}

// - - -

void InitAll(Handle<Object> target) {
  target->Set(NanNew<String>("getImageTags"), NanNew<FunctionTemplate>(GetImageTags)->GetFunction());
  target->Set(NanNew<String>("setImageTags"), NanNew<FunctionTemplate>(SetImageTags)->GetFunction());
  target->Set(NanNew<String>("deleteImageTags"), NanNew<FunctionTemplate>(DeleteImageTags)->GetFunction());
  target->Set(NanNew<String>("getImagePreviews"), NanNew<FunctionTemplate>(GetImagePreviews)->GetFunction());
}
NODE_MODULE(exiv2, InitAll)
