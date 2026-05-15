@0x8b678131d14d12e8;

# Motus form annotations
annotation table(struct) :Text;
annotation label(field) :Text;
annotation placeholder(field) :Text;
annotation computed(field) :Void;
annotation noInsert(field) :Void;
annotation noUpdate(field) :Void;
annotation formOptional(field) :Void;
annotation multiline(field) :Void;
annotation hidden(field) :Void;
annotation readonly(field) :Void;

enum Role {
  admin @0;
  editor @1;
  viewer @2;
}

struct Contact $table("contacts") {
  id        @0 :UInt64   $computed;
  name      @1 :Text     $label("Full Name");
  email     @2 :Text     $label("Email") $placeholder("user@example.com");
  age       @3 :UInt16   $formOptional;
  active    @4 :Bool = true;
  bio       @5 :Text     $multiline $label("Biography") $formOptional;
  role      @6 :Text     $noInsert;
  createdAt @7 :Int64    $computed;
  updatedAt @8 :Int64    $computed;
}

struct Product $table("products") {
  id          @0 :UInt64  $computed;
  name        @1 :Text    $label("Product Name");
  price       @2 :Float64 $label("Price") $placeholder("0.00");
  description @3 :Text    $multiline $label("Description");
  inStock     @4 :Bool = true;
}
