#include <v8.h>
#include <node.h>
#include <unistd.h>
#include <string>
#include <exiv2/image.hpp>
#include <exiv2/exif.hpp>

using namespace node;
using namespace v8;

#define DEBUGMODE 1
#define debug_printf(...) do {if(DEBUGMODE)printf(__VA_ARGS__);} while (0)

class Exiv2Node: ObjectWrap
{
  public:

  static Persistent<FunctionTemplate> s_ct;
  static void Init(Handle<Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    s_ct = Persistent<FunctionTemplate>::New(t);
    s_ct->InstanceTemplate()->SetInternalFieldCount(1);
    s_ct->SetClassName(String::NewSymbol("Exiv2Node"));

    NODE_SET_PROTOTYPE_METHOD(s_ct, "getImageTags", ImageTags);
    target->Set(String::NewSymbol("Exiv2Node"), s_ct->GetFunction());
  }

  Exiv2Node()
  {
  }

  ~Exiv2Node()
  {
  }

  static Handle<Value> New(const Arguments& args)
  {
    HandleScope scope;
    Exiv2Node* exiv2node = new Exiv2Node();
    exiv2node->Wrap(args.This());
    return args.This();
  }

  /* structure for passing our various objects around libeio */
  struct exiv2node_thread_data_t {
    Exiv2Node *exiv2node;
    Exiv2::Image::AutoPtr image;
    Persistent<Function> cb;
    std::string fileName;
  };

  static Handle<Value> ImageTags(const Arguments& args)
  {
    HandleScope scope;

    /* Usage arguments */
    if (args.Length() <= (1) || !args[1]->IsFunction())
      return ThrowException(Exception::TypeError(String::New("Usage: <filename> <callback function>")));

    Local<String> fileName = Local<String>::Cast(args[0]);
    Local<Function> cb = Local<Function>::Cast(args[1]);

    Exiv2Node* exiv2node = ObjectWrap::Unwrap<Exiv2Node>(args.This());

    /* Set up our thread data struct, pass off to the libeio thread pool */
    exiv2node_thread_data_t *thread_data = new exiv2node_thread_data_t();
    thread_data->exiv2node = exiv2node;
    thread_data->cb = Persistent<Function>::New(cb);
    thread_data->fileName = std::string(*String::AsciiValue(fileName));

    exiv2node->Ref();
    eio_custom(ImageTagsWorker, EIO_PRI_DEFAULT, AfterImageTags, thread_data);
    ev_ref(EV_DEFAULT_UC);

    return Undefined();
  }

  static int ImageTagsWorker(eio_req *req)
  {
    exiv2node_thread_data_t *thread_data = static_cast<exiv2node_thread_data_t *>(req->data);

    /* Exiv2 processing of the file.. */
    thread_data->image = Exiv2::ImageFactory::open(thread_data->fileName);
    thread_data->image->readMetadata();

    return 0;
  }

  /* Thread complete callback.. */
  static int AfterImageTags(eio_req *req)
  {
    HandleScope scope;
    exiv2node_thread_data_t *thread_data = static_cast<exiv2node_thread_data_t *>(req->data);
    ev_unref(EV_DEFAULT_UC);
    thread_data->exiv2node->Unref();

    Exiv2::ExifData &exifData = thread_data->image->exifData();
    Local<Value> argv[1];

    /* Create a V8 array with all the image tags and their corresponding values */
    if (exifData.empty() == false) {
		Local<Array> tags = Array::New();
    	Exiv2::ExifData::const_iterator end = exifData.end();
		for (Exiv2::ExifData::const_iterator i = exifData.begin(); i != end; ++i) {
			tags->Set(String::New(i->key().c_str()), String::New(i->value().toString().c_str()));
		}
		argv[0] = tags;
	}else {
		argv[0] = Local<Value>::New(Null());
	}

    /* Pass the argv array object to our callback function */
    TryCatch try_catch;
    thread_data->cb->Call(Context::GetCurrent()->Global(), 1, argv);
    if (try_catch.HasCaught()) {
      FatalException(try_catch);
    }

    thread_data->cb.Dispose();

    // Assuming std::auto_ptr does its job here and Exiv2::Image::AutoPtr is destroyed when it goes out of scope here..
    delete thread_data;
    return 0;
  }

};

Persistent<FunctionTemplate> Exiv2Node::s_ct;

extern "C" {
  static void init (Handle<Object> target)
  {
    Exiv2Node::Init(target);
  }

  NODE_MODULE(exiv2node, init);
}
