namespace render {

class FinalPass
{
  public:
    static int create(int a);

  private:
    int width_;
    int height_;
};

}

int area(int w, int h) {
    auto doubled = w * 2;
    return doubled * h;
}
